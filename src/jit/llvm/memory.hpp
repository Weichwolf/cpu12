// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/llvm/builder.hpp"

#include "llvm-c/Types.h"

namespace cpu12::detail::llvm19 {
class MemoryAccess {
public:
  MemoryAccess(Builder &builder, LLVMValueRef owner, Instruction instruction, LLVMValueRef value);
  void direct(const Memory &memory);
  [[nodiscard]] LLVMValueRef external();
  [[nodiscard]] LLVMValueRef join(LLVMValueRef external_value);

private:
  [[nodiscard]] LLVMBasicBlockRef append(const char *name);
  [[nodiscard]] LLVMValueRef pointer(LLVMValueRef base, std::size_t offset);
  [[nodiscard]] LLVMValueRef byte(LLVMValueRef base, std::size_t offset);
  void transfer(LLVMValueRef bytes, std::size_t high, std::size_t low);
  Builder &builder_;
  LLVMValueRef owner_, value_, function_;
  Instruction instruction_;
  LLVMBasicBlockRef external_, joined_, direct_ = nullptr;
  LLVMValueRef direct_value_ = nullptr;
};

[[nodiscard]] bool memory_operand(const Instruction &instruction) noexcept;
}
