// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/llvm/builder.hpp"

#include "llvm-c/Types.h"

#include <span>

namespace cpu12::detail::llvm19 {
class Flow;

struct Values {
  LLVMValueRef d;
  LLVMValueRef x;
  LLVMValueRef y;
  LLVMValueRef sp;
  LLVMValueRef ccr;
};

[[nodiscard]] LLVMValueRef register_value(Builder &b, Values &state, Instruction instruction);
void register_value(Builder &b, Values &state, Instruction instruction, LLVMValueRef value);
void lower(Builder &builder,
           Values &values,
           Instruction instruction,
           LLVMValueRef operand = nullptr);
[[nodiscard]] bool supports(const Instruction &instruction) noexcept;
void lift(LLVMValueRef function,
          const Flow &flow,
          std::span<const BlockInstruction> instructions,
          const Memory &memory);
}
