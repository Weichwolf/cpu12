// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::accumulator {
using Byte = std::uint8_t;
constexpr unsigned byte_mask = 0xff, byte_bits = 8, word_mask = 0xffff;
constexpr Registers seed{.pc = 0xfffe, .d = 0, .x = 0xa55a, .y = 0x5aa5, .sp = 0xffff, .ccr = 0xff};
inline std::uint64_t checked = 0, unqualified = 0;
enum class Operation : Byte { add, subtract, compare };

struct Input {
  Operation operation;
  Registers before;
};

struct Operands {
  unsigned left, right, carry = 0;
};

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline int signed_byte(const unsigned value) {
  constexpr unsigned sign = 0x80;
  return static_cast<int>(value) - (value < sign ? 0 : static_cast<int>(byte_mask + 1U));
}

inline Registers arithmetic(const Input point, const Operands operands) {
  auto wanted = point.before;
  const bool add = point.operation == Operation::add;
  const auto raw = add ? operands.left + operands.right + operands.carry
                       : operands.left + byte_mask + 1U - operands.right;
  const auto result = raw & byte_mask;
  const int signed_result = add ? signed_byte(operands.left) + signed_byte(operands.right) +
                                      static_cast<int>(operands.carry)
                                : signed_byte(operands.left) - signed_byte(operands.right);
  constexpr int minimum = -128;
  constexpr int maximum = 127;
  const bool overflow = signed_result < minimum || signed_result > maximum;
  const bool carry = add ? raw > byte_mask : operands.left < operands.right;
  const bool half = ((operands.left % 16U) + (operands.right % 16U) + operands.carry) >= 16U;
  constexpr unsigned upper = 0xd0;
  constexpr unsigned retain_half = 0xf0;
  constexpr unsigned sign = 0x80;
  wanted.ccr =
      static_cast<Byte>((wanted.ccr & (add ? upper : retain_half)) | (result >= sign ? 8U : 0U) |
                        (result == 0 ? 4U : 0U) | (overflow ? 2U : 0U) | (carry ? 1U : 0U) |
                        (add && half ? 0x20U : 0U));
  if (point.operation != Operation::compare) {
    wanted.d = static_cast<std::uint16_t>((result << byte_bits) | (wanted.d & byte_mask));
  }
  wanted.pc = static_cast<std::uint16_t>(wanted.pc + 2U);
  return wanted;
}

inline std::array<Byte, 2> encode(const Operation operation) {
  constexpr std::array<Byte, 3> instructions{6, 0x16, 0x17};
  constexpr Byte prefix = 0x18;
  return {prefix, instructions.at(static_cast<unsigned>(operation))};
}

inline void retired(ObservedCore &core,
                    const std::span<const Byte> code,
                    const Registers before,
                    const Registers wanted,
                    const unsigned defined_ccr = byte_mask) {
  const auto result = core.run_instruction(code);
  const auto actual = core.registers();
  auto defined = actual;
  defined.ccr = wanted.ccr;
  require(
      result.retired == 1 && result.fault == Fault::none && result.pc == before.pc &&
          result.opcode == code[0] && defined == wanted &&
          (actual.ccr & defined_ccr) == (wanted.ccr & defined_ccr),
      "Accumulator operation must match independent full-register and defined-flag expectations");
  require(
      actual == wanted,
      "DAA's architecturally undefined V follows the separately documented preservation policy");
  ++checked;
}

void decimal_cases(ObservedCore &core);
}
