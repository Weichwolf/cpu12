// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/lift.hpp"

#include "llvm-c/Types.h"

namespace cpu12::detail::llvm19 {
struct DecimalDomain {
  LLVMValueRef input, valid;
};

struct UnaryValue {
  LLVMValueRef value, ccr;
};

[[nodiscard]] UnaryValue
lower_unary(Builder &b, LLVMValueRef source, LLVMValueRef ccr, Instruction instruction);
[[nodiscard]] DecimalDomain decimal_domain(Builder &b, const Values &s);
void lower_register(Builder &b, Values &s, Instruction instruction);
void lower_transfer(Builder &b, Values &s, Instruction instruction);
[[nodiscard]] bool transfer_masks(Instruction instruction) noexcept;
}
