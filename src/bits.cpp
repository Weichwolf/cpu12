// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
std::optional<Instruction> decode_bits(const std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) { return {}; }
  constexpr unsigned first_indexed = 0x0c;
  constexpr unsigned last_indexed = 0x0f;
  constexpr unsigned first_extended = 0x1c;
  constexpr unsigned last_extended = 0x1f;
  constexpr unsigned first_direct = 0x4c;
  constexpr unsigned last_direct = 0x4f;
  const auto opcode = bytes[0];
  const bool indexed = opcode >= first_indexed && opcode <= last_indexed;
  const bool extended = opcode >= first_extended && opcode <= last_extended;
  const bool direct = opcode >= first_direct && opcode <= last_direct;
  if (!indexed && !extended && !direct) { return {}; }
  const bool branch = (opcode & 2U) != 0;
  const unsigned tail = branch ? 2 : 1;
  if (bytes.size() < 2 + tail) { return {}; }
  Instruction instruction{.kind = branch ? Kind::bit_branch : Kind::memory_bits,
                          .opcode = opcode,
                          .condition = static_cast<std::uint8_t>(opcode & 1U)};
  const auto prefix = bytes.first(bytes.size() - tail);
  if (indexed) {
    if (!decode_indexed(instruction, prefix) || instruction.indexed.indirect) { return {}; }
  } else {
    const unsigned length = extended ? 3 : 2;
    if (prefix.size() != length) { return {}; }
    instruction.addressing = extended ? Addressing::extended : Addressing::direct;
    constexpr unsigned byte_bits = 8;
    instruction.address = static_cast<std::uint16_t>(
        extended ? (static_cast<unsigned>(bytes[1]) << byte_bits) | bytes[2] : bytes[1]);
  }
  instruction.bit_mask = bytes[prefix.size()];
  if (branch) {
    constexpr unsigned sign = 0x80;
    constexpr unsigned extension = 0xff00;
    const auto raw = bytes.back();
    instruction.operand = static_cast<std::uint16_t>(raw < sign ? raw : extension | raw);
  }
  instruction.length = static_cast<std::uint8_t>(bytes.size());
  return instruction;
}
}
