// SPDX-License-Identifier: GPL-3.0-only
#include "cycles.hpp"

#include "cpu12/memory.hpp"
#include "instruction.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace cpu12::detail {
namespace {
unsigned index_class(const Indexed &indexed) {
  constexpr unsigned extended_mask = 0xe4;
  constexpr unsigned extended = 0xe0;
  constexpr unsigned indirect_mask = 0xe7;
  constexpr unsigned indirect_d = 0xe7;
  const auto post = indexed.postbyte;
  if ((post & extended_mask) == extended) {
    const auto mode = post & 3U;
    if (mode < 2) { return 1; }
    return mode == 2 ? 2U : 4U;
  }
  return (post & indirect_mask) == indirect_d ? 3U : 0U;
}

unsigned read_cycles(const Instruction &instruction) {
  switch (instruction.addressing) {
    case Addressing::immediate: return instruction.word ? 2U : 1U;
    case Addressing::direct:
    case Addressing::extended: return 3;
    case Addressing::indexed: {
      constexpr std::array cycles{3U, 3U, 4U, 6U, 6U};
      return cycles.at(index_class(instruction.indexed));
    }
    case Addressing::relative: throw std::logic_error("Relative CPU12 data operand");
    default: throw std::logic_error("Invalid CPU12 data operand");
  }
}

unsigned memory_cycles(const Instruction &instruction) {
  const auto kind = instruction.kind;
  if (kind == Kind::store) {
    if (instruction.addressing == Addressing::direct) { return 2; }
    if (instruction.addressing == Addressing::extended) { return 3; }
    constexpr std::array cycles{2U, 3U, 3U, 4U, 4U};
    return cycles.at(index_class(instruction.indexed));
  }
  if (kind == Kind::memory_unary) {
    if (instruction.operation == RegisterOp::test) { return read_cycles(instruction); }
    if (instruction.addressing == Addressing::extended) { return 4; }
    constexpr std::array cycles{3U, 4U, 5U, 5U, 5U};
    return cycles.at(index_class(instruction.indexed));
  }
  if (instruction.addressing == Addressing::direct) { return 4; }
  if (instruction.addressing == Addressing::extended) { return 5; }
  constexpr std::array cycles{4U, 5U, 6U, 0U, 0U};
  const auto result = cycles.at(index_class(instruction.indexed));
  if (result == 0) { throw std::logic_error("Indirect CPU12 bit operation"); }
  return result;
}

unsigned move_cycles(const Instruction &instruction) {
  const auto source = instruction.move_source.addressing;
  const auto target = instruction.move_destination.addressing;
  if (source == Addressing::immediate) {
    return instruction.word && target == Addressing::extended ? 5U : 4U;
  }
  return source == Addressing::extended && target == Addressing::extended ? 6U : 5U;
}

unsigned math_cycles(const Instruction &instruction) {
  switch (instruction.math) {
    case MathOp::multiply_byte: return 1;
    case MathOp::multiply_unsigned:
    case MathOp::multiply_signed: return 3;
    case MathOp::divide_unsigned:
    case MathOp::divide_signed:
    case MathOp::divide_extended:
    case MathOp::divide_extended_signed:
    case MathOp::divide_fractional: return instruction.length == 1 ? 11U : 12U;
    default: throw std::logic_error("Invalid CPU12 arithmetic timing");
  }
}

struct DataRequirement {
  bool word = false;
  bool write = false;
};

bool ordinary(const Memory &memory, const Address address, const DataRequirement access) {
  const unsigned width = access.word ? 2U : 1U;
  return std::ranges::any_of(memory.regions(), [&](const auto &region) {
    if ((access.write && !region.writable) || !region.known.empty() || address < region.first) {
      return false;
    }
    const auto offset = static_cast<std::size_t>(address - region.first);
    return offset <= region.bytes.size() && width <= region.bytes.size() - offset;
  });
}

bool ordinary_operand(const Memory &memory,
                      const MoveOperand &operand,
                      const DataRequirement access) {
  if (!access.write && operand.addressing == Addressing::immediate) { return true; }
  return operand.addressing == Addressing::extended && ordinary(memory, operand.value, access);
}

InstructionTiming derive_timing(const Instruction &instruction) {
  switch (instruction.kind) {
    case Kind::nop: return {.base = 1};
    case Kind::load:
    case Kind::add:
    case Kind::add_carry:
    case Kind::subtract:
    case Kind::subtract_carry:
    case Kind::compare:
    case Kind::bit_and:
    case Kind::bit_or:
    case Kind::bit_xor:
    case Kind::bit_test: return {.base = read_cycles(instruction)};
    case Kind::store:
    case Kind::memory_unary:
    case Kind::memory_bits:
    case Kind::bit_branch: return {.base = memory_cycles(instruction)};
    case Kind::branch:
      return instruction.length == 2 ? InstructionTiming{.base = 1, .taken_extra = 2}
                                     : InstructionTiming{.base = 3, .taken_extra = 1};
    case Kind::effective_address: return {.base = 2};
    case Kind::jump:
    case Kind::call: {
      constexpr std::array jump_cycles{3U, 3U, 4U, 6U, 6U};
      const auto base = instruction.addressing == Addressing::indexed
                            ? jump_cycles.at(index_class(instruction.indexed))
                            : 3U;
      return {.base = base + (instruction.kind == Kind::call ? 1U : 0U)};
    }
    case Kind::push: return {.base = 2};
    case Kind::pull: return {.base = 3};
    case Kind::return_subroutine: return {.base = 5};
    case Kind::register_operation:
      if (instruction.operation == RegisterOp::decimal_adjust) { return {.base = 3}; }
      return {.base = instruction.operation == RegisterOp::add_b ||
                              instruction.operation == RegisterOp::subtract_b ||
                              instruction.operation == RegisterOp::compare_b
                          ? 2U
                          : 1U};
    case Kind::transfer: {
      constexpr unsigned prefix = 0x18;
      return {.base = instruction.opcode == prefix ? 2U : 1U};
    }
    case Kind::loop: return {.base = 3};
    case Kind::move: return {.base = move_cycles(instruction)};
    case Kind::multiply_divide: return {.base = math_cycles(instruction)};
    case Kind::return_interrupt: return {.base = 8};
    case Kind::software_interrupt: return {.base = 9};
    case Kind::wait: return {.base = 7};
    case Kind::stop: return {.base = 2, .taken_extra = 6};
    default: throw std::logic_error("Invalid CPU12 instruction timing");
  }
}
}

InstructionTiming instruction_timing(const Instruction &instruction) {
  return instruction.cycles != 0 ? InstructionTiming{.base = instruction.cycles,
                                                     .taken_extra = instruction.taken_cycles}
                                 : derive_timing(instruction);
}

bool may_access_owner(const Instruction &instruction, const Memory &memory) {
  switch (instruction.kind) {
    case Kind::nop:
    case Kind::branch:
    case Kind::effective_address:
    case Kind::register_operation:
    case Kind::transfer:
    case Kind::loop:
    case Kind::multiply_divide: return false;
    case Kind::jump: return instruction.indexed.indirect;
    case Kind::call:
    case Kind::push:
    case Kind::pull:
    case Kind::return_subroutine:
    case Kind::return_interrupt:
    case Kind::software_interrupt:
    case Kind::wait:
    case Kind::stop: return true;
    case Kind::move:
      return !ordinary_operand(memory, instruction.move_source, {.word = instruction.word}) ||
             !ordinary_operand(
                 memory, instruction.move_destination, {.word = instruction.word, .write = true});
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
    case Kind::store:
    case Kind::memory_unary:
    case Kind::memory_bits:
    case Kind::bit_branch: break;
    default: throw std::logic_error("Invalid CPU12 access classification");
  }
  if (instruction.addressing == Addressing::immediate) { return false; }
  if (instruction.addressing == Addressing::indexed) { return true; }
  const bool write =
      instruction.kind == Kind::store || instruction.kind == Kind::memory_bits ||
      (instruction.kind == Kind::memory_unary && instruction.operation != RegisterOp::test);
  return !ordinary(memory, instruction.address, {.word = instruction.word, .write = write});
}

bool branch_taken(const Instruction &instruction, const std::uint8_t ccr) {
  const auto condition = instruction.condition;
  const bool carry = (ccr & 1U) != 0;
  const bool zero = (ccr & 4U) != 0;
  const bool negative = (ccr & 8U) != 0;
  const bool overflow = (ccr & 2U) != 0;
  const std::array conditions{true,
                              false,
                              !carry && !zero,
                              carry || zero,
                              !carry,
                              carry,
                              !zero,
                              zero,
                              !overflow,
                              overflow,
                              !negative,
                              negative,
                              negative == overflow,
                              negative != overflow,
                              !zero && negative == overflow,
                              zero || negative != overflow};
  return conditions.at(condition);
}
}
