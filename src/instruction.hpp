// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
enum class Kind : std::uint8_t {
  nop,
  load,
  add,
  subtract,
  compare,
  bit_and,
  bit_or,
  bit_xor,
  bit_test,
  branch
};

struct Instruction {
  Kind kind = Kind::nop;
  std::uint8_t opcode = 0, operand = 0, length = 0;
  bool accumulator_b = false;
};

[[nodiscard]] std::optional<Instruction> decode(std::span<const std::uint8_t> bytes);
}
