// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/code.hpp"

namespace cpu12::detail {
void transfer_instruction(Code &code, const Instruction &instruction);
}
