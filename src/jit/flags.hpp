// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "jit/code.hpp"

namespace cpu12::detail {
struct FlagPolicy {
  bool carry = false, overflow = false, half = false;
};

void flags(Code &code, FlagPolicy policy, bool publish = true);
void restore_carry(Code &code);
}
