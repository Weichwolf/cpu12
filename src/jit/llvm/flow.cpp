// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/flow.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/block.hpp"
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/lift.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Types.h"

#include <array>
#include <cstddef>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpu12::detail::llvm19 {
namespace {
LLVMValueRef function(LLVMModuleRef module, const std::string &name, const bool internal) {
  auto *context = LLVMGetModuleContext(module);
  auto *pointer = LLVMPointerTypeInContext(context, 0);
  auto *word = LLVMInt64TypeInContext(context);
  auto *count = LLVMInt32TypeInContext(context);
  std::array fields{word, word};
  auto *result = LLVMStructTypeInContext(context, fields.data(), fields.size(), 0);
  std::array parameters{pointer, pointer, count, count, word};
  const auto size = internal ? parameters.size() : 3U;
  auto *type = LLVMFunctionType(result, parameters.data(), static_cast<unsigned>(size), 0);
  auto *value = LLVMAddFunction(module, name.c_str(), type);
  if (internal) { LLVMSetLinkage(value, LLVMInternalLinkage); }
  return value;
}

LLVMValueRef constant(LLVMModuleRef module, const std::string &name, LLVMValueRef value) {
  auto *result = LLVMAddGlobal(module, LLVMTypeOf(value), name.c_str());
  LLVMSetInitializer(result, value);
  LLVMSetGlobalConstant(result, 1);
  LLVMSetLinkage(result, LLVMPrivateLinkage);
  LLVMSetUnnamedAddress(result, LLVMGlobalUnnamedAddr);
  return result;
}

LLVMValueRef wrapper(LLVMModuleRef module, const std::string &name, LLVMValueRef body) {
  auto *entry = function(module, name, false);
  auto *context = LLVMGetModuleContext(module);
  const Builder builder(context, LLVMAppendBasicBlockInContext(context, entry, "entry"));
  auto *registers = LLVMGetParam(entry, 0);
  auto *pc =
      builder.CreateGEP(builder.getInt8Ty(), registers, builder.getInt64(offsetof(Registers, pc)));
  auto *location =
      builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(), pc), builder.getInt64Ty());
  std::array arguments{
      registers, LLVMGetParam(entry, 1), LLVMGetParam(entry, 2), builder.getInt32(0), location};
  auto *call = LLVMBuildCall2(
      builder.get(), LLVMGlobalGetValueType(body), body, arguments.data(), arguments.size(), "");
  LLVMSetTailCallKind(call, LLVMTailCallKindTail);
  LLVMBuildRet(builder.get(), call);
  return entry;
}

std::array<LLVMValueRef, 5> arguments(LLVMValueRef function) {
  std::array<LLVMValueRef, 5> result{};
  LLVMGetParams(function, result.data());
  return result;
}
}

Flow::Flow(LLVMModuleRef module,
           const std::span<const std::vector<BlockInstruction>> regions,
           const Memory &memory,
           const std::string &prefix,
           const bool native)
    : module_(module), dispatch_(function(module, prefix + "dispatch", true)) {
  std::vector<LLVMValueRef> bodies;
  for (const auto &region : regions) {
    const auto name = prefix + std::to_string(region.front().pc);
    auto *body = function(module, name + ".body", true);
    auto *entry = wrapper(module, name, body);
    bodies.push_back(body);
    if (native) {
      native_attributes(entry);
      native_attributes(body);
    }
    for (const auto &operation : region) {
      if (!targets_.emplace(operation.pc, body).second) {
        throw std::invalid_argument("CPU12 native flow has duplicate instruction entries");
      }
    }
  }
  if (native) { native_attributes(dispatch_); }
  emit_dispatch(table(prefix));
  for (std::size_t index = 0; index < regions.size(); ++index) {
    lift(bodies[index], *this, regions[index], memory);
  }
}

LLVMValueRef Flow::target(const Address pc) const {
  const auto found = targets_.find(pc);
  return found == targets_.end() ? nullptr : found->second;
}

void Flow::transfer(const Builder &builder,
                    LLVMValueRef target,
                    const std::span<LLVMValueRef> arguments) const {
  auto *call = LLVMBuildCall2(builder.get(),
                              LLVMGlobalGetValueType(dispatch_),
                              target,
                              arguments.data(),
                              static_cast<unsigned>(arguments.size()),
                              "");
  LLVMSetTailCallKind(call, LLVMTailCallKindMustTail);
  LLVMBuildRet(builder.get(), call);
}

LLVMValueRef Flow::table(const std::string &prefix) const {
  auto *pointer = LLVMPointerTypeInContext(LLVMGetModuleContext(module_), 0);
  std::array<LLVMValueRef, native_page_entries> top{};
  top.fill(LLVMConstNull(pointer));
  std::map<std::size_t, decltype(top)> pages;
  for (const auto &[pc, body] : targets_) {
    const auto page = std::size_t{pc} >> native_page_shift;
    const auto [found, inserted] = pages.try_emplace(page);
    if (inserted) { found->second.fill(LLVMConstNull(pointer)); }
    found->second[pc % native_page_entries] = body;
  }
  for (auto &[page, values] : pages) {
    top[page] = constant(module_,
                         prefix + "page." + std::to_string(page),
                         LLVMConstArray2(pointer, values.data(), values.size()));
  }
  return constant(module_, prefix + "pages", LLVMConstArray2(pointer, top.data(), top.size()));
}

void Flow::emit_dispatch(LLVMValueRef table) const {
  auto *context = LLVMGetModuleContext(module_);
  const Builder builder(context, LLVMAppendBasicBlockInContext(context, dispatch_, "entry"));
  auto *missing = LLVMAppendBasicBlockInContext(context, dispatch_, "missing");
  auto *page_found = LLVMAppendBasicBlockInContext(context, dispatch_, "page");
  auto *ready = LLVMAppendBasicBlockInContext(context, dispatch_, "ready");
  auto *pointer = LLVMPointerTypeInContext(context, 0);
  auto *array = LLVMArrayType2(pointer, native_page_entries);
  auto args = arguments(dispatch_);
  auto *pc_address =
      builder.CreateGEP(builder.getInt8Ty(), args[0], builder.getInt64(offsetof(Registers, pc)));
  auto *pc = builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(), pc_address),
                                builder.getInt64Ty());
  std::array page_index{builder.getInt64(0),
                        builder.CreateLShr(pc, builder.getInt64(native_page_shift))};
  auto *page = builder.CreateLoad(
      pointer,
      LLVMBuildGEP2(builder.get(), array, table, page_index.data(), page_index.size(), ""));
  LLVMBuildCondBr(
      builder.get(), builder.CreateICmpEQ(page, LLVMConstNull(pointer)), missing, page_found);
  LLVMPositionBuilderAtEnd(builder.get(), page_found);
  std::array offset{builder.getInt64(0),
                    builder.CreateAnd(pc, builder.getInt64(native_page_entries - 1))};
  auto *entry = builder.CreateLoad(
      pointer, LLVMBuildGEP2(builder.get(), array, page, offset.data(), offset.size(), ""));
  LLVMBuildCondBr(
      builder.get(), builder.CreateICmpEQ(entry, LLVMConstNull(pointer)), missing, ready);
  LLVMPositionBuilderAtEnd(builder.get(), ready);
  transfer(builder, entry, args);
  LLVMPositionBuilderAtEnd(builder.get(), missing);
  auto *result = LLVMGetUndef(LLVMGetReturnType(LLVMGlobalGetValueType(dispatch_)));
  result = LLVMBuildInsertValue(
      builder.get(), result, builder.CreateZExt(args[3], builder.getInt64Ty()), 0, "");
  LLVMBuildRet(builder.get(), LLVMBuildInsertValue(builder.get(), result, args[4], 1, ""));
}
}
