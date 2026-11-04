// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
namespace {
bool memory_operand(Instruction &instruction, const std::span<const std::uint8_t> bytes) {
  const bool extended = instruction.addressing == Addressing::extended;
  const unsigned length = extended ? 3U : 2U;
  if (bytes.size() != length) { return false; }
  instruction.length = static_cast<std::uint8_t>(length);
  constexpr unsigned byte_bits = 8;
  instruction.address = static_cast<std::uint16_t>(
      extended ? (static_cast<unsigned>(bytes[1]) << byte_bits) | bytes[2] : bytes[1]);
  return true;
}

bool arithmetic_kind(Instruction &instruction) {
  enum Opcode : std::uint8_t {
    load_a = 0x86,
    add_a = 0x8b,
    sub_a = 0x80,
    cmp_a = 0x81,
    and_a = 0x84,
    or_a = 0x8a,
    xor_a = 0x88,
    test_a = 0x85
  };

  constexpr unsigned arithmetic_mask = 0x8f;
  switch (instruction.opcode & arithmetic_mask) {
    case load_a: instruction.kind = Kind::load; break;
    case add_a: instruction.kind = Kind::add; break;
    case sub_a: instruction.kind = Kind::subtract; break;
    case cmp_a: instruction.kind = Kind::compare; break;
    case and_a: instruction.kind = Kind::bit_and; break;
    case or_a: instruction.kind = Kind::bit_or; break;
    case xor_a: instruction.kind = Kind::bit_xor; break;
    case test_a: instruction.kind = Kind::bit_test; break;
    default: return false;
  }
  return true;
}

bool word_kind(Instruction &instruction) {
  constexpr unsigned word_mask = 0xcf;
  constexpr unsigned word_load = 0xcc;
  constexpr unsigned word_compare = 0x8c;
  constexpr unsigned target_mask = 3;
  constexpr unsigned add_d = 0xc3;
  constexpr unsigned sub_d = 0x83;
  const auto operation = instruction.opcode & word_mask;
  if ((operation & ~target_mask) == word_load) {
    instruction.kind = Kind::load;
    instruction.target = static_cast<WordTarget>(operation & target_mask);
  } else if ((operation & ~target_mask) == word_compare) {
    instruction.kind = Kind::compare;
    instruction.target = static_cast<WordTarget>(operation & target_mask);
  } else if (operation == add_d) {
    instruction.kind = Kind::add;
  } else if (operation == sub_d) {
    instruction.kind = Kind::subtract;
  } else {
    return false;
  }
  instruction.word = true;
  return true;
}

bool word_store(Instruction &instruction) {
  constexpr unsigned word_store_mask = 0xdc;
  constexpr unsigned store_d = 0x5c;
  constexpr unsigned target_mask = 3;
  constexpr unsigned extended_store = 0x20;
  if ((instruction.opcode & word_store_mask) != store_d) { return false; }
  instruction.kind = Kind::store;
  instruction.word = true;
  instruction.target = static_cast<WordTarget>(instruction.opcode & target_mask);
  instruction.addressing =
      (instruction.opcode & extended_store) != 0 ? Addressing::extended : Addressing::direct;
  return true;
}

bool immediate_operand(Instruction &instruction, const std::span<const std::uint8_t> bytes) {
  const unsigned length = instruction.word ? 3U : 2U;
  if (bytes.size() != length) { return false; }
  constexpr unsigned byte_bits = 8;
  instruction.operand = static_cast<std::uint16_t>(
      instruction.word ? (static_cast<unsigned>(bytes[1]) << byte_bits) | bytes[2] : bytes[1]);
  instruction.length = static_cast<std::uint8_t>(length);
  return true;
}

bool arithmetic_operand(Instruction &instruction, const std::span<const std::uint8_t> bytes) {
  constexpr unsigned addressing_mask = 0x30;
  constexpr unsigned indexed_mode = 0x20;
  constexpr unsigned extended_mode = 0x30;
  const auto mode = instruction.opcode & addressing_mask;
  if (mode == indexed_mode) { return decode_indexed(instruction, bytes); }
  if (mode == 0) {
    if (!immediate_operand(instruction, bytes)) { return false; }
  } else {
    instruction.addressing = mode == extended_mode ? Addressing::extended : Addressing::direct;
    if (!memory_operand(instruction, bytes)) { return false; }
  }
  return true;
}

}

std::optional<Instruction> decode(const std::span<const std::uint8_t> bytes) {
  constexpr unsigned nop = 0xa7;
  constexpr unsigned first_branch = 0x20;
  constexpr unsigned last_branch = 0x2f;
  constexpr unsigned accumulator_b = 0x40;
  constexpr unsigned store_mask = 0xde;
  constexpr unsigned store_a = 0x5a;
  constexpr unsigned extended_store = 0x20;
  if (bytes.empty()) { return {}; }
  const auto opcode = bytes[0];
  if (opcode == nop && bytes.size() == 1) {
    return Instruction{.kind = Kind::nop, .opcode = opcode, .length = 1};
  }
  Instruction instruction{.opcode = opcode};
  if (opcode >= first_branch && opcode <= last_branch) {
    if (bytes.size() != 2) { return {}; }
    instruction.kind = Kind::branch;
    instruction.operand = bytes[1];
    instruction.length = 2;
    return instruction;
  }
  constexpr unsigned first_indexed_store = 0x6a;
  constexpr unsigned last_indexed_store = 0x6f;
  constexpr unsigned first_word_store = 0x6c;
  if (opcode >= first_indexed_store && opcode <= last_indexed_store) {
    instruction.kind = Kind::store;
    instruction.word = opcode >= first_word_store;
    instruction.target = static_cast<WordTarget>(opcode & 3U);
    instruction.accumulator_b = (opcode & 1U) != 0;
    if (!decode_indexed(instruction, bytes)) { return {}; }
    return instruction;
  }
  if (word_store(instruction)) {
    if (!memory_operand(instruction, bytes)) { return {}; }
    return instruction;
  }
  if ((opcode & store_mask) == store_a) {
    instruction.kind = Kind::store;
    instruction.accumulator_b = (opcode & 1U) != 0;
    instruction.addressing =
        (opcode & extended_store) != 0 ? Addressing::extended : Addressing::direct;
    if (!memory_operand(instruction, bytes)) { return {}; }
    return instruction;
  }
  if (!word_kind(instruction) && !arithmetic_kind(instruction)) { return {}; }
  instruction.accumulator_b = (opcode & accumulator_b) != 0;
  if (!arithmetic_operand(instruction, bytes)) { return {}; }
  return instruction;
}
}
