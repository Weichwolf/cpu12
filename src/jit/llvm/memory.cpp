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
                           const AccessValues values)
    : builder_(builder),
      owner_(owner),
      value_(values.value),
      function_(LLVMGetBasicBlockParent(LLVMGetInsertBlock(builder.get()))),
      address_(values.address),
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

LLVMValueRef MemoryAccess::element(LLVMValueRef base, LLVMValueRef offset) {
  return builder_.CreateGEP(
      builder_.getInt8Ty(), base, builder_.CreateZExt(offset, builder_.getInt64Ty()));
}

LLVMValueRef MemoryAccess::byte(LLVMValueRef base, LLVMValueRef offset) {
  return builder_.CreateZExt(builder_.CreateLoad(builder_.getInt8Ty(), element(base, offset)),
                             builder_.getInt32Ty());
}

LLVMValueRef MemoryAccess::transfer(LLVMValueRef bytes, const Offsets offsets) {
  if (instruction_.kind == Kind::store) {
    auto *first =
        instruction_.word ? builder_.CreateLShr(value_, builder_.getInt32(byte_bits)) : value_;
    builder_.CreateStore(builder_.CreateTrunc(first, builder_.getInt8Ty()),
                         element(bytes, offsets.high));
    if (instruction_.word) {
      builder_.CreateStore(builder_.CreateTrunc(value_, builder_.getInt8Ty()),
                           element(bytes, offsets.low));
    }
    return value_;
  }
  auto *result = byte(bytes, offsets.high);
  if (instruction_.word) {
    result = builder_.CreateOr(builder_.CreateShl(result, builder_.getInt32(byte_bits)),
                               byte(bytes, offsets.low));
  }
  return result;
}

void MemoryAccess::region(const Ram &region, const std::size_t index) {
  constexpr unsigned word_mask = 0xffff;
  const bool write = instruction_.kind == Kind::store;
  auto *next_address = builder_.CreateAnd(builder_.CreateAdd(address_, builder_.getInt32(1)),
                                          builder_.getInt32(word_mask));
  const Offsets offsets{.high = builder_.CreateSub(address_, builder_.getInt32(region.first)),
                        .low = builder_.CreateSub(next_address, builder_.getInt32(region.first))};
  auto *fits = builder_.CreateICmpULT(offsets.high, builder_.getInt32(region.bytes.size()));
  if (instruction_.word) {
    fits = builder_.CreateAnd(
        fits, builder_.CreateICmpULT(offsets.low, builder_.getInt32(region.bytes.size())));
  }
  auto *body = append("mapped_memory");
  auto *next = append("next_region");
  LLVMBuildCondBr(builder_.get(), fits, body, next);
  LLVMPositionBuilderAtEnd(builder_.get(), body);
  auto *type = LLVMTypeOf(owner_);
  auto *bindings =
      builder_.CreateLoad(type, pointer(owner_, offsetof(NativeContext, memory_bindings)));
  auto *binding = pointer(bindings, index * sizeof(MemoryBinding));
  auto *bytes = builder_.CreateLoad(type, pointer(binding, offsetof(MemoryBinding, bytes)));
  LLVMValueRef known = nullptr;
  if (!region.known.empty()) {
    known = builder_.CreateLoad(type, pointer(binding, offsetof(MemoryBinding, known)));
    if (!write) {
      auto *ready = builder_.CreateICmpNE(byte(known, offsets.high), builder_.getInt32(0));
      if (instruction_.word) {
        ready = builder_.CreateAnd(
            ready, builder_.CreateICmpNE(byte(known, offsets.low), builder_.getInt32(0)));
      }
      auto *initialized = append("known_memory");
      LLVMBuildCondBr(builder_.get(), ready, initialized, external_);
      LLVMPositionBuilderAtEnd(builder_.get(), initialized);
    }
  }
  auto *value = transfer(bytes, offsets);
  if (write && known != nullptr) {
    builder_.CreateStore(builder_.getInt8(1), element(known, offsets.high));
    if (instruction_.word) {
      builder_.CreateStore(builder_.getInt8(1), element(known, offsets.low));
    }
  }
  direct_.emplace_back(value, LLVMGetInsertBlock(builder_.get()));
  LLVMBuildBr(builder_.get(), joined_);
  LLVMPositionBuilderAtEnd(builder_.get(), next);
}

void MemoryAccess::direct(const Memory &memory) {
  const auto regions = memory.regions();
  const auto next = static_cast<Address>(instruction_.address + 1U);
  for (std::size_t index = 0; index < regions.size(); ++index) {
    const auto &region = regions[index];
    const bool write = instruction_.kind == Kind::store;
    if (region.bytes.empty() || (write && !region.writable)) { continue; }
    if (instruction_.addressing == Addressing::indexed) {
      this->region(region, index);
      continue;
    }
    if (instruction_.address < region.first) { continue; }
    const auto high = static_cast<std::size_t>(instruction_.address - region.first);
    const auto low = static_cast<std::size_t>(next - region.first);
    if (high >= region.bytes.size() ||
        (instruction_.word && (next < region.first || low >= region.bytes.size()))) {
      continue;
    }
    this->region(region, index);
    break;
  }
  LLVMBuildBr(builder_.get(), external_);
  LLVMPositionBuilderAtEnd(builder_.get(), external_);
}

LLVMValueRef MemoryAccess::external() {
  const bool write = instruction_.kind == Kind::store;
  const bool selected = instruction_.kind == Kind::bit_branch;
  const unsigned count = write || selected ? 3U : 2U;
  auto *module = LLVMGetGlobalParent(function_);
  auto *context = LLVMGetModuleContext(module);
  const char *name = nullptr;
  if (write) {
    name = instruction_.word ? "cpu12_write_word" : "cpu12_write_byte";
  } else if (selected) {
    name = "cpu12_read_masked_byte";
  } else {
    name = instruction_.word ? "cpu12_read_word" : "cpu12_read_byte";
  }
  auto *value_type = instruction_.word ? builder_.getInt16Ty() : builder_.getInt8Ty();
  std::array parameters{LLVMTypeOf(owner_), builder_.getInt16Ty(), value_type};
  auto *type = LLVMFunctionType(builder_.getInt32Ty(), parameters.data(), count, 0);
  auto *function = LLVMGetNamedFunction(module, name);
  if (function == nullptr) {
    function = LLVMAddFunction(module, name, type);
    for (unsigned index = 2; index <= count; ++index) {
      LLVMAddAttributeAtIndex(
          function,
          index,
          LLVMCreateEnumAttribute(context, LLVMGetEnumAttributeKindForName("zeroext", 7), 0));
    }
  }
  LLVMValueRef last = nullptr;
  if (write) { last = builder_.CreateTrunc(value_, value_type); }
  if (selected) { last = builder_.getInt8(instruction_.bit_mask); }
  std::array arguments{owner_, builder_.CreateTrunc(address_, builder_.getInt16Ty()), last};
  auto *call = LLVMBuildCall2(builder_.get(), type, function, arguments.data(), count, "");
  for (unsigned index = 2; index <= count; ++index) {
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
  auto *io_value = instruction_.kind == Kind::store ? value_ : external_value;
  LLVMAddIncoming(result, &io_value, &external_block, 1);
  for (auto &[value, block] : direct_) { LLVMAddIncoming(result, &value, &block, 1); }
  return result;
}

bool memory_operand(const Instruction &instruction) noexcept {
  return instruction.addressing == Addressing::direct ||
         instruction.addressing == Addressing::extended ||
         instruction.addressing == Addressing::indexed;
}
}
