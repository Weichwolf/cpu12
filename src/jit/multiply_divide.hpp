// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/code.hpp"

namespace cpu12::detail {
void multiply_divide_instruction(Code &code, const Instruction &instruction);
void divide_instruction(Code &code, const Instruction &instruction);
}
