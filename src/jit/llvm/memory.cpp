// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/memory.hpp"

#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/llvm/builder.hpp"
#include "runtime.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Types.h"

#include <array>
#include <cstddef>

namespace cpu12::detail::llvm19 {
namespace {
constexpr unsigned byte_bits = 8;
}

MemoryAccess::MemoryAccess(Builder &builder,
                           LLVMValueRef owner,
                           const Instruction instruction,
                           LLVMValueRef value)
    : builder_(builder),
      owner_(owner),
      value_(value),
      function_(LLVMGetBasicBlockParent(LLVMGetInsertBlock(builder.get()))),
      instruction_(instruction),
      external_(append("io")),
      joined_(append("memory_done")) {}

LLVMBasicBlockRef MemoryAccess::append(const char *name) {
  return LLVMAppendBasicBlockInContext(
      LLVMGetModuleContext(LLVMGetGlobalParent(function_)), function_, name);
}

LLVMValueRef MemoryAccess::pointer(LLVMValueRef base, const std::size_t offset) {
  return builder_.CreateGEP(builder_.getInt8Ty(), base, builder_.getInt64(offset));
}

LLVMValueRef MemoryAccess::byte(LLVMValueRef base, const std::size_t offset) {
  return builder_.CreateZExt(builder_.CreateLoad(builder_.getInt8Ty(), pointer(base, offset)),
                             builder_.getInt32Ty());
}

void MemoryAccess::transfer(LLVMValueRef bytes, const std::size_t high, const std::size_t low) {
  if (instruction_.kind == Kind::store) {
    auto *first =
        instruction_.word ? builder_.CreateLShr(value_, builder_.getInt32(byte_bits)) : value_;
    builder_.CreateStore(builder_.CreateTrunc(first, builder_.getInt8Ty()), pointer(bytes, high));
    if (instruction_.word) {
      builder_.CreateStore(builder_.CreateTrunc(value_, builder_.getInt8Ty()), pointer(bytes, low));
    }
    direct_value_ = value_;
  } else {
    direct_value_ = byte(bytes, high);
    if (instruction_.word) {
      direct_value_ = builder_.CreateOr(
          builder_.CreateShl(direct_value_, builder_.getInt32(byte_bits)), byte(bytes, low));
    }
  }
}

void MemoryAccess::direct(const Memory &memory) {
  const auto regions = memory.regions();
  const auto next = static_cast<Address>(instruction_.address + 1U);
  for (std::size_t index = 0; index < regions.size(); ++index) {
    const auto &region = regions[index];
    const bool write = instruction_.kind == Kind::store;
    if ((write && !region.writable) || instruction_.address < region.first) { continue; }
    const auto high = static_cast<std::size_t>(instruction_.address - region.first);
    const auto low = static_cast<std::size_t>(next - region.first);
    if (high >= region.bytes.size() ||
        (instruction_.word && (next < region.first || low >= region.bytes.size()))) {
      continue;
    }
    auto *type = LLVMTypeOf(owner_);
    auto *bindings =
        builder_.CreateLoad(type, pointer(owner_, offsetof(NativeContext, memory_bindings)));
    auto *binding = pointer(bindings, index * sizeof(MemoryBinding));
    auto *bytes = builder_.CreateLoad(type, pointer(binding, offsetof(MemoryBinding, bytes)));
    LLVMValueRef known = nullptr;
    if (!region.known.empty()) {
      known = builder_.CreateLoad(type, pointer(binding, offsetof(MemoryBinding, known)));
      if (!write) {
        auto *ready = builder_.CreateICmpNE(byte(known, high), builder_.getInt32(0));
        if (instruction_.word) {
          ready = builder_.CreateAnd(ready,
                                     builder_.CreateICmpNE(byte(known, low), builder_.getInt32(0)));
        }
        auto *body = append("known_memory");
        LLVMBuildCondBr(builder_.get(), ready, body, external_);
        LLVMPositionBuilderAtEnd(builder_.get(), body);
      }
    }
    transfer(bytes, high, low);
    if (write && known != nullptr) {
      builder_.CreateStore(builder_.getInt8(1), pointer(known, high));
      if (instruction_.word) { builder_.CreateStore(builder_.getInt8(1), pointer(known, low)); }
    }
    direct_ = LLVMGetInsertBlock(builder_.get());
    LLVMBuildBr(builder_.get(), joined_);
    LLVMPositionBuilderAtEnd(builder_.get(), external_);
    return;
  }
  LLVMBuildBr(builder_.get(), external_);
  LLVMPositionBuilderAtEnd(builder_.get(), external_);
}

LLVMValueRef MemoryAccess::external() {
  const bool write = instruction_.kind == Kind::store;
  auto *module = LLVMGetGlobalParent(function_);
  auto *context = LLVMGetModuleContext(module);
  const char *name = nullptr;
  if (write) {
    name = instruction_.word ? "cpu12_write_word" : "cpu12_write_byte";
  } else {
    name = instruction_.word ? "cpu12_read_word" : "cpu12_read_byte";
  }
  auto *value_type = instruction_.word ? builder_.getInt16Ty() : builder_.getInt8Ty();
  std::array parameters{LLVMTypeOf(owner_), builder_.getInt16Ty(), value_type};
  auto *type = LLVMFunctionType(builder_.getInt32Ty(), parameters.data(), write ? 3U : 2U, 0);
  auto *function = LLVMGetNamedFunction(module, name);
  if (function == nullptr) {
    function = LLVMAddFunction(module, name, type);
    for (unsigned index = 2; index <= (write ? 3U : 2U); ++index) {
      LLVMAddAttributeAtIndex(
          function,
          index,
          LLVMCreateEnumAttribute(context, LLVMGetEnumAttributeKindForName("zeroext", 7), 0));
    }
  }
  std::array arguments{owner_,
                       builder_.getInt16(instruction_.address),
                       write ? builder_.CreateTrunc(value_, value_type) : nullptr};
  auto *call =
      LLVMBuildCall2(builder_.get(), type, function, arguments.data(), write ? 3U : 2U, "");
  for (unsigned index = 2; index <= (write ? 3U : 2U); ++index) {
    LLVMAddCallSiteAttribute(
        call,
        index,
        LLVMCreateEnumAttribute(context, LLVMGetEnumAttributeKindForName("zeroext", 7), 0));
  }
  return call;
}

LLVMValueRef MemoryAccess::join(LLVMValueRef external_value) {
  auto *external_block = LLVMGetInsertBlock(builder_.get());
  LLVMBuildBr(builder_.get(), joined_);
  LLVMPositionBuilderAtEnd(builder_.get(), joined_);
  auto *result = LLVMBuildPhi(builder_.get(), builder_.getInt32Ty(), "operand");
  auto *value = instruction_.kind == Kind::store ? value_ : external_value;
  LLVMAddIncoming(result, &value, &external_block, 1);
  if (direct_ != nullptr) { LLVMAddIncoming(result, &direct_value_, &direct_, 1); }
  return result;
}

bool memory_operand(const Instruction &instruction) noexcept {
  return instruction.addressing == Addressing::direct ||
         instruction.addressing == Addressing::extended;
}
}
