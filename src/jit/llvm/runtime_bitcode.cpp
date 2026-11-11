// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/runtime_bitcode.hpp"

#include "jit/llvm/lifetime.hpp"

#include "llvm-c/BitReader.h"
#include "llvm-c/Core.h"
#include "llvm-c/Types.h"

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>

namespace cpu12::detail::llvm19 {
std::span<const std::byte> runtime_bitcode() noexcept {
  static constexpr auto bytes = std::to_array(
#include "runtime_bitcode.inc"
  );
  return std::as_bytes(std::span(bytes).first(bytes.size() - 1));
}

TargetDescription runtime_target() {
  const Owner<LLVMContextRef, LLVMContextDispose> context(LLVMContextCreate());
  const auto bytes = runtime_bitcode();
  const Owner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> buffer(
      LLVMCreateMemoryBufferWithMemoryRangeCopy(
          reinterpret_cast<const char *>(bytes.data()), bytes.size(), "cpu12-runtime"));
  LLVMModuleRef raw = nullptr;
  const auto failed = LLVMParseBitcodeInContext2(context.get(), buffer.get(), &raw);
  const Owner<LLVMModuleRef, LLVMDisposeModule> module(raw);
  if (failed != 0 || !module) { throw std::runtime_error("Cannot read the CPU12 adapter target"); }
  return {.triple = LLVMGetTarget(module.get()), .layout = LLVMGetDataLayoutStr(module.get())};
}
}
