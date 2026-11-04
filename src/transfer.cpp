// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
std::optional<Instruction> decode_transfer(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 2) { return {}; }
  constexpr unsigned transfer = 0xb7;
  constexpr unsigned prefix = 0x18;
  constexpr unsigned tab = 0x0e;
  constexpr unsigned tba = 0x0f;
  constexpr unsigned selector_mask = 7;
  constexpr unsigned source_shift = 4;
  constexpr unsigned factory_selector = 3;
  const auto opcode = bytes[0];
  std::uint16_t postbyte = bytes[1];
  if (opcode == prefix) {
    if (postbyte == tab) {
      postbyte = 1;
    } else if (postbyte == tba) {
      postbyte = 1U << source_shift;
    } else {
      return {};
    }
  } else if (opcode != transfer) {
    return {};
  }
  const auto source = (postbyte >> source_shift) & selector_mask;
  const auto destination = postbyte & selector_mask;
  if (source == factory_selector || destination == factory_selector) { return {}; }
  return Instruction{.kind = Kind::transfer, .opcode = opcode, .length = 2, .operand = postbyte};
}
}
