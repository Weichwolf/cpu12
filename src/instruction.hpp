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
  branch,
  store
};

enum class Addressing : std::uint8_t { immediate, direct, extended };

enum class WordTarget : std::uint8_t { d, y, x, sp };

struct Instruction {
  Kind kind = Kind::nop;
  std::uint8_t opcode = 0, length = 0;
  std::uint16_t operand = 0;
  bool accumulator_b = false;
  Addressing addressing = Addressing::immediate;
  std::uint16_t address = 0;
  bool word = false;
  WordTarget target = WordTarget::d;
};

[[nodiscard]] std::optional<Instruction> decode(std::span<const std::uint8_t> bytes);
}
