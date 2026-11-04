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
}

std::optional<Instruction> decode(const std::span<const std::uint8_t> bytes) {
  constexpr unsigned nop = 0xa7;
  constexpr unsigned first_branch = 0x20;
  constexpr unsigned last_branch = 0x2f;
  constexpr unsigned accumulator_b = 0x40;
  constexpr unsigned store_mask = 0xde;
  constexpr unsigned store_a = 0x5a;
  constexpr unsigned extended_store = 0x20;
  constexpr unsigned addressing_mask = 0x30;
  constexpr unsigned indexed_mode = 0x20;
  constexpr unsigned extended_mode = 0x30;
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
  if ((opcode & store_mask) == store_a) {
    instruction.kind = Kind::store;
    instruction.accumulator_b = (opcode & 1U) != 0;
    instruction.addressing =
        (opcode & extended_store) != 0 ? Addressing::extended : Addressing::direct;
    if (!memory_operand(instruction, bytes)) { return {}; }
    return instruction;
  }
  if (!arithmetic_kind(instruction)) { return {}; }
  instruction.accumulator_b = (opcode & accumulator_b) != 0;
  const auto mode = opcode & addressing_mask;
  if (mode == indexed_mode) { return {}; }
  if (mode == 0) {
    if (bytes.size() != 2) { return {}; }
    instruction.operand = bytes[1];
    instruction.length = 2;
  } else {
    instruction.addressing = mode == extended_mode ? Addressing::extended : Addressing::direct;
    if (!memory_operand(instruction, bytes)) { return {}; }
  }
  return instruction;
}
}
