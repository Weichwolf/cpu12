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
  store,
  effective_address
};

enum class Addressing : std::uint8_t { immediate, direct, extended, indexed };
enum class IndexBase : std::uint8_t { x, y, sp, pc };
enum class IndexOffset : std::uint8_t { constant, a, b, d };

struct Indexed {
  IndexBase base = IndexBase::x;
  IndexOffset offset = IndexOffset::constant;
  std::uint16_t displacement = 0;
  std::uint8_t postbyte = 0;
  std::int8_t update = 0;
  bool indirect = false;
};

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
  Indexed indexed{};
};

[[nodiscard]] bool decode_indexed(Instruction &instruction, std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<Instruction> decode(std::span<const std::uint8_t> bytes);
}
