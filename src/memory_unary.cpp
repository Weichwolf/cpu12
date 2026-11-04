// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
std::optional<Instruction> decode_memory_unary(const std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) { return {}; }
  constexpr unsigned first_indexed = 0x60;
  constexpr unsigned last_indexed = 0x69;
  constexpr unsigned first_extended = 0x70;
  constexpr unsigned last_extended = 0x79;
  constexpr unsigned indexed_test = 0xe7;
  constexpr unsigned extended_test = 0xf7;
  constexpr unsigned low_mask = 15;
  const auto opcode = bytes[0];
  const bool indexed =
      (opcode >= first_indexed && opcode <= last_indexed) || opcode == indexed_test;
  const bool extended =
      (opcode >= first_extended && opcode <= last_extended) || opcode == extended_test;
  if (!indexed && !extended) { return {}; }
  constexpr std::array operations{RegisterOp::negate,
                                  RegisterOp::complement,
                                  RegisterOp::increment,
                                  RegisterOp::decrement,
                                  RegisterOp::logical_right,
                                  RegisterOp::rotate_left,
                                  RegisterOp::rotate_right,
                                  RegisterOp::arithmetic_right,
                                  RegisterOp::arithmetic_left,
                                  RegisterOp::clear};
  const auto operation = opcode == indexed_test || opcode == extended_test
                             ? RegisterOp::test
                             : operations[opcode & low_mask];
  Instruction instruction{.kind = Kind::memory_unary, .opcode = opcode, .operation = operation};
  if (indexed) {
    if (!decode_indexed(instruction, bytes)) { return {}; }
  } else {
    if (bytes.size() != 3) { return {}; }
    constexpr unsigned byte_bits = 8;
    instruction.addressing = Addressing::extended;
    instruction.length = 3;
    instruction.address =
        static_cast<std::uint16_t>((static_cast<unsigned>(bytes[1]) << byte_bits) | bytes[2]);
  }
  return instruction;
}
}
