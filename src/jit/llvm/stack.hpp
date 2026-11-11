// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/memory.hpp"

#include "llvm-c/Types.h"

namespace cpu12::detail::llvm19 {
[[nodiscard]] bool stack_operand(const Instruction &instruction) noexcept;
[[nodiscard]] bool frame_operand(const Instruction &instruction) noexcept;
[[nodiscard]] LLVMValueRef swap_frame_word(Builder &b, LLVMValueRef value);
[[nodiscard]] LLVMValueRef stack_register(Builder &b, const Values &state, StackTarget target);
void stack_register(Builder &b, Values &state, StackTarget target, LLVMValueRef value);
[[nodiscard]] LLVMValueRef stack_address(Builder &b, const Values &state, int delta);

template <class Transfer>
void push_frame(Builder &b, const Values &state, LLVMValueRef pc, Transfer transfer) {
  const Instruction word{.kind = Kind::store, .addressing = Addressing::indexed, .word = true};
  const Instruction byte{.kind = Kind::store, .addressing = Addressing::indexed};
  const auto write = [&](const int offset, LLVMValueRef value, const Instruction access) {
    static_cast<void>(
        transfer(access, AccessValues{.address = stack_address(b, state, offset), .value = value}));
  };
  write(-2, pc, word);
  write(-4, state.y, word);
  write(-6, state.x, word);
  write(-8, swap_frame_word(b, state.d), word);
  write(-9, state.ccr, byte);
}
}
