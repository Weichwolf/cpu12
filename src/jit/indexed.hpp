// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"

namespace cpu12::detail {
// Computed address arithmetic and known RAM access are emitted host instructions.
// Only genuine external accesses call the supplied bus owner.
void indexed_read(Code &code, const Instruction &instruction, Ram ram);
void indexed_write(Code &code, const Instruction &instruction, Ram ram);
void jump(Code &code, const Instruction &instruction, Ram ram);
void indexed_effective_address(Code &code, const Instruction &instruction);
}
