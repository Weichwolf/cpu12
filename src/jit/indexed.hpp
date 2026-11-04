// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"

#include <cstddef>

namespace cpu12::detail {
struct Access {
  bool word = false, write = false;
};

// Byte admission leaves the native offset in r10. These helpers emit code only.
[[nodiscard]] std::size_t outside_byte_ram(Code &code, Ram ram);
void native_data_access(Code &code, Ram ram, Access access);

void data_access(Code &code, Ram ram, Access access);
void indexed_address(Code &code, const Instruction &instruction, Ram ram);
void indexed_commit(Code &code, const Instruction &instruction);

// Computed address arithmetic and known RAM access are emitted host instructions.
// Only genuine external accesses call the supplied bus owner.
void indexed_read(Code &code, const Instruction &instruction, Ram ram);
void indexed_write(Code &code, const Instruction &instruction, Ram ram);
void jump(Code &code, const Instruction &instruction, Ram ram);
void indexed_effective_address(Code &code, const Instruction &instruction);
}
