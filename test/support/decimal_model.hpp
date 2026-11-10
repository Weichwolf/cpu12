// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "accumulator_model.hpp"
#include "cpu12/core.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace cpu12::test::accumulator {
inline constexpr std::array<Byte, 2> decimal_code{0x18, 7};
inline constexpr unsigned half_flag = 0x20;

struct Row {
  bool carry, half;
  std::array<unsigned, 2> high, low;
  unsigned correction;
  bool result_carry;
};

inline constexpr std::array<Row, 9> rows{{{.carry = false,
                                           .half = false,
                                           .high = {0, 9},
                                           .low = {0, 9},
                                           .correction = 0,
                                           .result_carry = false},
                                          {.carry = false,
                                           .half = false,
                                           .high = {0, 8},
                                           .low = {10, 15},
                                           .correction = 6,
                                           .result_carry = false},
                                          {.carry = false,
                                           .half = true,
                                           .high = {0, 9},
                                           .low = {0, 3},
                                           .correction = 6,
                                           .result_carry = false},
                                          {.carry = false,
                                           .half = false,
                                           .high = {10, 15},
                                           .low = {0, 9},
                                           .correction = 0x60,
                                           .result_carry = true},
                                          {.carry = false,
                                           .half = false,
                                           .high = {9, 15},
                                           .low = {10, 15},
                                           .correction = 0x66,
                                           .result_carry = true},
                                          {.carry = false,
                                           .half = true,
                                           .high = {10, 15},
                                           .low = {0, 3},
                                           .correction = 0x66,
                                           .result_carry = true},
                                          {.carry = true,
                                           .half = false,
                                           .high = {0, 2},
                                           .low = {0, 9},
                                           .correction = 0x60,
                                           .result_carry = true},
                                          {.carry = true,
                                           .half = false,
                                           .high = {0, 2},
                                           .low = {10, 15},
                                           .correction = 0x66,
                                           .result_carry = true},
                                          {.carry = true,
                                           .half = true,
                                           .high = {0, 3},
                                           .low = {0, 3},
                                           .correction = 0x66,
                                           .result_carry = true}}};

inline std::optional<Row> rule(const Registers before) {
  const unsigned a = before.d >> byte_bits;
  const auto high = a / 16U;
  const auto low = a % 16U;
  const bool carry = (before.ccr & 1U) != 0;
  const bool half = (before.ccr & half_flag) != 0;
  for (const auto row : rows) {
    if (row.carry == carry && row.half == half && high >= row.high[0] && high <= row.high[1] &&
        low >= row.low[0] && low <= row.low[1]) {
      return row;
    }
  }
  return {};
}

struct Decimal {
  unsigned a;
  bool carry;
};

inline Registers decimal_result(const Registers before, const Decimal result) {
  auto wanted = before;
  wanted.d = static_cast<std::uint16_t>((result.a << byte_bits) | (before.d & byte_mask));
  wanted.pc = static_cast<std::uint16_t>(before.pc + decimal_code.size());
  constexpr unsigned preserved = 0xf2;
  constexpr unsigned sign = 0x80;
  wanted.ccr = static_cast<Byte>((before.ccr & preserved) | (result.a >= sign ? 8U : 0U) |
                                 (result.a == 0 ? 4U : 0U) | static_cast<unsigned>(result.carry));
  return wanted;
}

inline unsigned actual_input(const Registers before) {
  constexpr unsigned carry_index = 0x100;
  constexpr unsigned half_index = 0x200;
  return (before.d >> byte_bits) | ((before.ccr & 1U) != 0 ? carry_index : 0U) |
         ((before.ccr & half_flag) != 0 ? half_index : 0U);
}

}
