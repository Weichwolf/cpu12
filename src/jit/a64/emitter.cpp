// SPDX-License-Identifier: GPL-3.0-only
#include "jit/a64/emitter.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/block.hpp"
#include "jit/emitter.hpp"
#include "jit/interrupt.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpu12::detail::a64 {
namespace {
constexpr std::uint32_t bit(const Reg value) {
  return std::uint32_t{1} << static_cast<unsigned>(value);
}

std::vector<std::pair<Reg, Reg>> saved(const std::uint32_t used) {
  constexpr std::array pairs{Reg::x19, Reg::x21, Reg::x23, Reg::x25, Reg::x27};
  std::vector<std::pair<Reg, Reg>> result;
  for (const auto first : pairs) {
    const auto second = static_cast<Reg>(static_cast<unsigned>(first) + 1);
    if ((used & (bit(first) | bit(second))) != 0) {
      result.emplace_back((used & bit(first)) != 0 ? first : Reg::zr,
                          (used & bit(second)) != 0 ? second : Reg::zr);
    }
  }
  return result;
}

constexpr std::array fields{std::pair{pc, offsetof(Registers, pc)},
                            std::pair{d, offsetof(Registers, d)},
                            std::pair{x, offsetof(Registers, x)},
                            std::pair{y, offsetof(Registers, y)},
                            std::pair{stack, offsetof(Registers, sp)}};
constexpr std::size_t location_offset = 24;
}

Code Emitter::prologue(const std::uint32_t used) const {
  Code entry;
  entry.push(Reg::fp, Reg::lr);
  entry.add(Reg::fp, Reg::sp, 0, Width::x64);
  for (const auto &[first, second] : saved(used)) { entry.push(first, second); }
  constexpr unsigned local_bytes = 32;
  entry.subtract(Reg::sp, Reg::sp, local_bytes, Width::x64);
  entry.move(registers, Reg::x0);
  entry.move(context, Reg::x1);
  for (const auto &[reg, offset] : fields) {
    if ((used & input_registers_ & bit(reg)) != 0) {
      entry.load(reg, registers, offset, Access::half);
    }
  }
  if ((used & bit(ccr)) != 0) {
    entry.load(ccr, registers, offsetof(Registers, ccr), Access::byte);
  }
  return entry;
}

void Emitter::begin(const EntryMode mode) {
  begin_offset_ = code_.size();
  // Admitted resident paths define retirement and the exit before any possible fault.
  if (mode == EntryMode::resident) { return; }
  code_.constant(retired, 0);
  if (mode == EntryMode::block) { code_.move(remaining, Reg::x2, Width::w32); }
  code_.store(Reg::zr, Reg::sp, location_offset, Access::word);
}

void Emitter::publish() {
  const auto dirty = code_.written_registers();
  for (const auto &[reg, offset] : fields) {
    if ((dirty & bit(reg)) != 0) { code_.store(reg, registers, offset, Access::half); }
  }
  if ((dirty & bit(ccr)) != 0) {
    code_.store(ccr, registers, offsetof(Registers, ccr), Access::byte);
  }
}

std::vector<Byte> Emitter::finish() {
  code_.branch(done_);
  code_.bind(failure_);
  code_.load(Reg::x1, Reg::sp, location_offset);
  code_.constant(Reg::x17, native_boundary_mask);
  code_.alu(Alu::bit_or, Reg::x1, Reg::x1, Reg::x17);
  code_.store(Reg::x1, Reg::sp, location_offset);
  code_.bind(done_);
  publish();
  code_.move(Reg::x0, retired);
  code_.load(Reg::x1, Reg::sp, location_offset);
  const auto used = code_.used_registers() | bit(registers) | bit(context);
  const auto preserved = saved(used);
  constexpr unsigned local_bytes = 32;
  code_.add(Reg::sp, Reg::sp, local_bytes, Width::x64);
  for (const auto &[first, second] : std::views::reverse(preserved)) { code_.pop(first, second); }
  code_.pop(Reg::fp, Reg::lr);
  code_.return_();
  const auto entry = prologue(used);
  code_.splice(begin_offset_, entry.bytes());
  return code_.bytes();
}

void Emitter::publish_flags() {
  if (policy_.flags_live) { code_.move(ccr, Reg::x4, Width::w32); }
}

void Emitter::location(const Instruction &operation) {
  const bool boundary = operation.kind == Kind::wait || operation.kind == Kind::stop;
  code_.constant(Reg::x17,
                 (std::uint32_t{operation.opcode} << native_opcode_shift) |
                     (boundary ? native_boundary_mask : 0U));
  code_.alu(Alu::bit_or, Reg::x4, pc, Reg::x17);
  code_.store(Reg::x4, Reg::sp, location_offset);
}

void Emitter::mask(const Reg target, const Reg source, const std::uint32_t bits) {
  code_.constant(Reg::x17, bits);
  code_.alu(Alu::bit_and, target, source, Reg::x17);
}

void Emitter::add_word(const Reg target, const Reg source, const int amount) {
  constexpr int maximum = 0xfff;
  if (amount >= 0 && amount <= maximum) {
    code_.add(target, source, static_cast<std::uint16_t>(amount));
  } else if (amount < 0 && amount >= -maximum) {
    code_.subtract(target, source, static_cast<std::uint16_t>(-amount));
  } else {
    code_.constant(Reg::x17, static_cast<Address>(amount));
    code_.alu(Alu::add, target, source, Reg::x17);
  }
  code_.extract(target, target, 0, word_bits);
}

void Emitter::advance(const unsigned amount) {
  if (!policy_.advance_pc) { return; }
  add_word(pc, pc, static_cast<int>(amount));
}

Reg Emitter::word_target(const WordTarget target) {
  constexpr std::array targets{d, y, x, stack};
  return targets.at(static_cast<unsigned>(target));
}

Reg Emitter::index_base(const IndexBase base) {
  constexpr std::array bases{x, y, stack, pc};
  return bases.at(static_cast<unsigned>(base));
}

void Emitter::load_target(const Reg target, const Instruction &operation) {
  code_.extract(target,
                operation.word ? word_target(operation.target) : d,
                operation.word || operation.accumulator_b != swapped_d_ ? 0U : byte_bits,
                operation.word ? word_bits : byte_bits);
}

void Emitter::store_target(const Reg value, const Instruction &operation) {
  if (operation.word) {
    code_.extract(word_target(operation.target), value, 0, word_bits);
  } else {
    code_.insert(d, value, operation.accumulator_b != swapped_d_ ? 0U : byte_bits, byte_bits);
  }
}

void Emitter::state_byte(const StateField field, const Byte value) {
  code_.constant(Reg::x17, value);
  code_.store(Reg::x17,
              context,
              offsetof(NativeContext, cpu) + static_cast<std::size_t>(field),
              Access::byte);
}

void Emitter::operation(const Instruction &instruction) {
  switch (instruction.kind) {
    case Kind::load:
    case Kind::add:
    case Kind::add_carry:
    case Kind::subtract:
    case Kind::subtract_carry:
    case Kind::compare:
    case Kind::bit_and:
    case Kind::bit_or:
    case Kind::bit_xor:
    case Kind::bit_test:
    case Kind::store: arithmetic(instruction); break;
    case Kind::nop: break;
    case Kind::branch: branch(instruction); return;
    case Kind::loop: loop(instruction); return;
    case Kind::effective_address:
      address(instruction);
      commit_index(instruction);
      code_.move(word_target(instruction.target), Reg::x9, Width::w32);
      break;
    case Kind::jump:
      address(instruction);
      commit_index(instruction);
      code_.move(pc, Reg::x9, Width::w32);
      return;
    case Kind::register_operation: unary(instruction); break;
    case Kind::transfer: transfer(instruction); break;
    case Kind::memory_unary:
    case Kind::memory_bits:
    case Kind::bit_branch: memory_modify(instruction); return;
    case Kind::move: move(instruction); break;
    case Kind::push:
    case Kind::pull:
    case Kind::return_subroutine: stack_operation(instruction); return;
    case Kind::call: subroutine(instruction); return;
    case Kind::return_interrupt: return_interrupt(); return;
    case Kind::wait:
    case Kind::stop: wait(instruction); return;
    case Kind::multiply_divide: math(instruction); break;
    case Kind::software_interrupt: throw std::logic_error("SWI requires its exception owner");
    default: throw std::logic_error("Unknown CPU12 A64 instruction kind");
  }
  advance(instruction.length);
}

std::vector<Byte> Emitter::instruction(const Instruction &operation) {
  begin(EntryMode::single);
  location(operation);
  this->operation(operation);
  code_.add(retired, retired, 1);
  return finish();
}

}

namespace cpu12::detail {
std::vector<std::uint8_t> emit(const Instruction &instruction, const Memory &ram) {
  if (instruction.kind == Kind::software_interrupt) {
    constexpr Address software_vector = 0xfff6;
    return a64::Emitter(ram).interrupt(
        {.vector = software_vector}, InterruptFrame::push, instruction.length);
  }
  return a64::Emitter(ram).instruction(instruction);
}

std::vector<std::uint8_t> emit_block(const std::span<const BlockInstruction> instructions,
                                     const Memory &memory) {
  return a64::Emitter(memory).block(instructions);
}

std::vector<std::uint8_t> emit_interrupt(const InterruptRequest request,
                                         const Memory &ram,
                                         const InterruptFrame frame,
                                         const std::uint8_t next_pc) {
  return a64::Emitter(ram).interrupt(request, frame, next_pc);
}

std::vector<std::uint8_t> emit_stop_wake() {
  return a64::Emitter(Memory::external()).wake();
}
}
