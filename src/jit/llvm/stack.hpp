// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/lift.hpp"

#include "llvm-c/Types.h"

namespace cpu12::detail::llvm19 {
[[nodiscard]] bool stack_operand(const Instruction &instruction) noexcept;
[[nodiscard]] bool frame_operand(const Instruction &instruction) noexcept;
[[nodiscard]] LLVMValueRef swap_frame_word(Builder &b, LLVMValueRef value);
[[nodiscard]] LLVMValueRef stack_register(Builder &b, const Values &state, StackTarget target);
void stack_register(Builder &b, Values &state, StackTarget target, LLVMValueRef value);
[[nodiscard]] LLVMValueRef stack_address(Builder &b, const Values &state, int delta);
}
