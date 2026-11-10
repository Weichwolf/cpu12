// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/llvm/builder.hpp"

#include "llvm-c/Types.h"

#include <utility>
#include <vector>

namespace cpu12::detail::llvm19 {
struct AccessValues {
  LLVMValueRef address, value = nullptr;
};

class MemoryAccess {
public:
  MemoryAccess(Builder &builder, LLVMValueRef owner, Instruction instruction, AccessValues values);
  void direct(const Memory &memory);
  [[nodiscard]] LLVMValueRef external();
  [[nodiscard]] LLVMValueRef join(LLVMValueRef external_value);

private:
  struct Offsets {
    LLVMValueRef high, low;
  };

  [[nodiscard]] LLVMBasicBlockRef append(const char *name);
  [[nodiscard]] LLVMValueRef pointer(LLVMValueRef base, std::size_t offset);
  [[nodiscard]] LLVMValueRef byte(LLVMValueRef base, LLVMValueRef offset);
  [[nodiscard]] LLVMValueRef element(LLVMValueRef base, LLVMValueRef offset);
  [[nodiscard]] LLVMValueRef transfer(LLVMValueRef bytes, Offsets offsets);
  void region(const Ram &region, std::size_t index);
  Builder &builder_;
  LLVMValueRef owner_, value_, function_, address_;
  Instruction instruction_;
  LLVMBasicBlockRef external_, joined_;
  std::vector<std::pair<LLVMValueRef, LLVMBasicBlockRef>> direct_;
};

[[nodiscard]] bool memory_operand(const Instruction &instruction) noexcept;
}
