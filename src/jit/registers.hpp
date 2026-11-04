// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/code.hpp"

namespace cpu12::detail {
void unary_operation(Code &code, const Instruction &instruction);
// With publish=false, AL receives the complete candidate CCR without a CPU write.
void unary_flags(Code &code, const Instruction &instruction, bool publish = true);

void register_instruction(Code &code, const Instruction &instruction);
}
