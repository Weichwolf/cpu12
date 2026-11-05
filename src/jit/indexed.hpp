// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace cpu12::detail {
struct Access {
  bool word = false, write = false;
  bool selected = false;
  std::uint8_t mask = std::numeric_limits<std::uint8_t>::max();
};

// Byte admission leaves the native offset in r10. These helpers emit code only.
[[nodiscard]] std::vector<std::size_t> admit_ram(Code &code, Ram ram, Access access);
void native_data_access(Code &code, Ram ram, Access access);

void data_access(Code &code, const Memory &memory, Access access);
void indexed_address(Code &code, const Instruction &instruction, const Memory &ram);
void indexed_commit(Code &code, const Instruction &instruction);

// Computed address arithmetic and known RAM access are emitted host instructions.
// Accesses not fully admitted by a storage grant call the actual I/O owner.
void indexed_read(Code &code, const Instruction &instruction, const Memory &ram);
void indexed_write(Code &code, const Instruction &instruction, const Memory &ram);
void jump(Code &code, const Instruction &instruction, const Memory &ram);
void indexed_effective_address(Code &code, const Instruction &instruction);
}
