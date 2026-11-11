// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"

namespace cpu12::detail::llvm19 {
class Builder;
struct Values;
void lower_math(Builder &builder, Values &state, MathOp operation);
}
