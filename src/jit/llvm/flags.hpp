// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "jit/llvm/builder.hpp"

#include "llvm-c/Types.h"

namespace cpu12::detail::llvm19 {
struct FlagInputs {
  LLVMValueRef value, carry_bit, overflow_bit;
  unsigned width;
};

[[nodiscard]] LLVMValueRef flags(Builder &b, LLVMValueRef old, FlagInputs input);
}
