// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/llvm/builder.hpp"

#include "llvm-c/Types.h"

#include <span>

namespace cpu12::detail::llvm19 {
struct Values {
  LLVMValueRef d;
  LLVMValueRef x;
  LLVMValueRef y;
  LLVMValueRef sp;
  LLVMValueRef ccr;
};

void lower(Builder &builder, Values &values, Instruction instruction);
[[nodiscard]] bool supports(const Instruction &instruction) noexcept;
[[nodiscard]] LLVMValueRef lift(LLVMModuleRef module,
                                LLVMContextRef context,
                                std::span<const BlockInstruction> instructions,
                                const char *name);
}
