// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/memory.hpp"

#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"
#include "runtime.hpp"

#include <cstdint>

namespace cpu12::detail::a64 {
void Emitter::address(const Instruction &operation) {
  if (operation.addressing != Addressing::indexed) {
    code_.constant(Reg::x9, operation.address);
    return;
  }
  const auto &indexed = operation.indexed;
  code_.move(Reg::x9, index_base(indexed.base), Width::w32);
  if (indexed.base == IndexBase::pc) { add_word(Reg::x9, Reg::x9, operation.length); }
  if (indexed.offset == IndexOffset::constant) {
    add_word(Reg::x9, Reg::x9, indexed.displacement);
  } else {
    code_.extract(Reg::x10,
                  d,
                  indexed.offset == IndexOffset::a ? byte_bits : 0,
                  indexed.offset == IndexOffset::d ? word_bits : byte_bits);
    code_.alu(Alu::add, Reg::x9, Reg::x9, Reg::x10);
    code_.extract(Reg::x9, Reg::x9, 0, word_bits);
  }
  if (indexed.indirect) {
    data_access({.word = true});
    code_.move(Reg::x9, Reg::x1, Width::w32);
  }
}

void Emitter::commit_index(const Instruction &operation) {
  if (operation.addressing == Addressing::indexed && operation.indexed.update != 0) {
    const auto base = index_base(operation.indexed.base);
    add_word(base, base, operation.indexed.update);
  }
}

void Emitter::ram_access(const Ram ram, const DataAccess access, const MemoryExit exits) {
  auto &next = exits.next;
  auto &done = exits.done;
  code_.constant(Reg::x17, ram.first);
  code_.alu(Alu::subtract, Reg::x10, Reg::x9, Reg::x17);
  code_.constant(Reg::x17, ram.bytes.size());
  code_.compare(Reg::x10, Reg::x17);
  code_.branch_far(Condition::carry, next);
  if (access.word) {
    add_word(Reg::x12, Reg::x9, 1);
    code_.constant(Reg::x17, ram.first);
    code_.alu(Alu::subtract, Reg::x12, Reg::x12, Reg::x17);
    code_.constant(Reg::x17, ram.bytes.size());
    code_.compare(Reg::x12, Reg::x17);
    code_.branch_far(Condition::carry, next);
  }
  if (!access.write && !ram.known.empty()) {
    code_.constant(Reg::x11, reinterpret_cast<std::uintptr_t>(ram.known.data()));
    code_.alu(Alu::add, Reg::x13, Reg::x11, Reg::x10, Width::x64);
    code_.load(Reg::x14, Reg::x13, 0, Access::byte);
    code_.branch_zero_far(Reg::x14, next);
    if (access.word) {
      code_.alu(Alu::add, Reg::x13, Reg::x11, Reg::x12, Width::x64);
      code_.load(Reg::x14, Reg::x13, 0, Access::byte);
      code_.branch_zero_far(Reg::x14, next);
    }
  }
  code_.constant(Reg::x11, reinterpret_cast<std::uintptr_t>(ram.bytes.data()));
  code_.alu(Alu::add, Reg::x13, Reg::x11, Reg::x10, Width::x64);
  if (access.write) {
    if (access.word) {
      code_.shift(Shift::right, Reg::x14, Reg::x0, byte_bits);
      code_.store(Reg::x14, Reg::x13, 0, Access::byte);
      code_.alu(Alu::add, Reg::x13, Reg::x11, Reg::x12, Width::x64);
    }
    code_.store(Reg::x0, Reg::x13, 0, Access::byte);
  } else {
    code_.load(Reg::x1, Reg::x13, 0, Access::byte);
    if (access.word) {
      code_.shift(Shift::left, Reg::x1, Reg::x1, byte_bits);
      code_.alu(Alu::add, Reg::x13, Reg::x11, Reg::x12, Width::x64);
      code_.load(Reg::x14, Reg::x13, 0, Access::byte);
      code_.alu(Alu::bit_or, Reg::x1, Reg::x1, Reg::x14);
    }
    if (access.selected) { mask(Reg::x1, Reg::x1, access.mask); }
  }
  if (access.write && !ram.known.empty()) {
    code_.constant(Reg::x11, reinterpret_cast<std::uintptr_t>(ram.known.data()));
    code_.constant(Reg::x14, 1);
    code_.alu(Alu::add, Reg::x13, Reg::x11, Reg::x10, Width::x64);
    code_.store(Reg::x14, Reg::x13, 0, Access::byte);
    if (access.word) {
      code_.alu(Alu::add, Reg::x13, Reg::x11, Reg::x12, Width::x64);
      code_.store(Reg::x14, Reg::x13, 0, Access::byte);
    }
  }
  code_.branch(done);
}

void Emitter::bus_access(const DataAccess access) {
  publish();
  if (access.write) { code_.extract(Reg::x2, Reg::x0, 0, access.word ? word_bits : byte_bits); }
  code_.move(Reg::x0, context);
  code_.move(Reg::x1, Reg::x9, Width::w32);
  if (access.write) {
    code_.constant(Reg::x16,
                   access.word ? reinterpret_cast<std::uintptr_t>(&write_word)
                               : reinterpret_cast<std::uintptr_t>(&write_byte));
  } else if (access.selected) {
    code_.constant(Reg::x17, std::uint32_t{access.mask} << word_bits);
    code_.alu(Alu::bit_or, Reg::x1, Reg::x1, Reg::x17);
    code_.constant(Reg::x16, reinterpret_cast<std::uintptr_t>(&read_masked_byte));
  } else {
    code_.constant(Reg::x16,
                   access.word ? reinterpret_cast<std::uintptr_t>(&read_word)
                               : reinterpret_cast<std::uintptr_t>(&read_byte));
  }
  code_.call(Reg::x16);
  if (access.write) {
    code_.branch_zero_far(Reg::x0, failure_);
  } else {
    code_.constant(Reg::x17, access.word ? failed_word_read : failed_read);
    code_.compare(Reg::x0, Reg::x17);
    code_.branch_far(Condition::equal, failure_);
    code_.move(Reg::x1, Reg::x0, Width::w32);
  }
}

void Emitter::data_access(const DataAccess access) {
  Label done;
  for (const auto ram : memory_.regions()) {
    if (ram.bytes.empty() || (access.write && !ram.writable)) { continue; }
    Label next;
    ram_access(ram, access, {.next = next, .done = done});
    code_.bind(next);
  }
  bus_access(access);
  code_.bind(done);
}

void Emitter::operand(const Instruction &operation) {
  if (operation.addressing == Addressing::immediate) {
    code_.constant(Reg::x1, operation.operand);
  } else {
    address(operation);
    data_access({.word = operation.word});
    commit_index(operation);
  }
}
}
