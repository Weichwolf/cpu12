// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/lift.hpp"

#include "llvm-c/Types.h"

namespace cpu12::detail::llvm19 {
[[nodiscard]] LLVMValueRef
effective_address(Builder &builder, Values &state, const BlockInstruction &operation);
void commit_index(Builder &builder, Values &state, const Instruction &instruction);
}
