// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"

namespace cpu12::detail {
void stack_instruction(Code &code, const Instruction &instruction, Ram ram);
void subroutine_call(Code &code, const Instruction &instruction, Ram ram);
}
