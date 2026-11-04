// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
std::optional<Instruction> decode(const std::span<const std::uint8_t> bytes) {
  constexpr unsigned nop = 0xa7;
  constexpr unsigned first_branch = 0x20;
  constexpr unsigned last_branch = 0x2f;
  constexpr unsigned accumulator_b = 0x40;
  constexpr unsigned immediate_group_mask = 0xbf;

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

  if (bytes.empty()) { return {}; }
  const auto opcode = bytes[0];
  if (opcode == nop && bytes.size() == 1) {
    return Instruction{.kind = Kind::nop, .opcode = opcode, .length = 1};
  }
  if (bytes.size() != 2) { return {}; }
  Instruction instruction{.opcode = opcode, .operand = bytes[1], .length = 2};
  if (opcode >= first_branch && opcode <= last_branch) {
    instruction.kind = Kind::branch;
    return instruction;
  }
  instruction.accumulator_b = (opcode & accumulator_b) != 0;
  switch (opcode & immediate_group_mask) {
    case load_a: instruction.kind = Kind::load; break;
    case add_a: instruction.kind = Kind::add; break;
    case sub_a: instruction.kind = Kind::subtract; break;
    case cmp_a: instruction.kind = Kind::compare; break;
    case and_a: instruction.kind = Kind::bit_and; break;
    case or_a: instruction.kind = Kind::bit_or; break;
    case xor_a: instruction.kind = Kind::bit_xor; break;
    case test_a: instruction.kind = Kind::bit_test; break;
    default: return {};
  }
  return instruction;
}
}
