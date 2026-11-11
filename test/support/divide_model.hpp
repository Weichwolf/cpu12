// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "multiply_divide_model.hpp"

#include <cstdint>

namespace cpu12::test::math::divide_model {
inline bool extended(const Operation operation) {
  return operation == Operation::ediv || operation == Operation::edivs;
}

inline bool signed_operation(const Operation operation) {
  return operation == Operation::idivs || operation == Operation::edivs;
}

inline Expected zero(const Input point) {
  auto wanted = point.before;
  constexpr unsigned defined_error = 0xf1;
  unsigned defined = defined_error;
  bool undefined_d = false;
  if (point.operation == Operation::idiv || point.operation == Operation::fdiv) {
    wanted.x = word_mask;
    constexpr unsigned retained = 0xf8;
    wanted.ccr =
        static_cast<Byte>((wanted.ccr & retained) | (point.operation == Operation::fdiv ? 3U : 1U));
    defined = byte_mask;
    undefined_d = true;
  } else {
    wanted.ccr = static_cast<Byte>(wanted.ccr | 1U);
  }
  return {.registers = wanted, .defined_ccr = defined, .undefined_d = undefined_d};
}

inline Expected fraction_overflow(const Input point) {
  auto wanted = point.before;
  wanted.x = word_mask;
  constexpr unsigned retained = 0xf8;
  wanted.ccr = static_cast<Byte>((wanted.ccr & retained) | 2U);
  return {.registers = wanted, .undefined_d = true};
}

struct Result {
  std::int64_t quotient, remainder;
};

inline Result division(const Input point) {
  const auto operation = point.operation;
  const auto divisor = signed_operation(operation) ? signed_word(point.before.x) : point.before.x;
  std::int64_t numerator = point.before.d;
  if (extended(operation)) {
    const auto raw = (static_cast<std::uint64_t>(point.before.y) << word_bits) | point.before.d;
    constexpr std::uint64_t long_sign = 0x80000000;
    numerator = static_cast<std::int64_t>(raw);
    if (operation == Operation::edivs && raw >= long_sign) {
      numerator -= static_cast<std::int64_t>(long_mask + 1U);
    }
  } else if (operation == Operation::idivs) {
    numerator = signed_word(point.before.d);
  } else if (operation == Operation::fdiv) {
    numerator *= word_mask + 1U;
  }
  const auto quotient = numerator / divisor;
  return {.quotient = quotient, .remainder = numerator - (quotient * divisor)};
}

inline Expected finite(const Input point) {
  const auto operation = point.operation;
  if (operation == Operation::fdiv && point.before.x <= point.before.d) {
    return fraction_overflow(point);
  }
  const auto result = division(point);
  constexpr std::int64_t signed_min = -32768;
  constexpr std::int64_t signed_max = 32767;
  const bool overflow = signed_operation(operation)
                            ? result.quotient < signed_min || result.quotient > signed_max
                            : result.quotient > word_mask;
  auto wanted = point.before;
  if (extended(operation) && overflow) {
    constexpr unsigned retained = 0xfe;
    constexpr unsigned defined = 0xf3;
    wanted.ccr = static_cast<Byte>((wanted.ccr & retained) | 2U);
    return {.registers = wanted, .defined_ccr = defined};
  }
  const auto quotient = static_cast<std::uint16_t>(result.quotient);
  wanted.d = static_cast<std::uint16_t>(result.remainder);
  if (extended(operation)) {
    wanted.y = quotient;
  } else {
    wanted.x = quotient;
  }
  const bool nz = extended(operation) || operation == Operation::idivs;
  constexpr unsigned retain_upper = 0xf0;
  constexpr unsigned retain_negative = 0xf8;
  constexpr unsigned word_sign = 0x8000;
  const auto n = nz && quotient >= word_sign ? 8U : 0U;
  wanted.ccr = static_cast<Byte>((wanted.ccr & (nz ? retain_upper : retain_negative)) | n |
                                 (quotient == 0 ? 4U : 0U) | (overflow ? 2U : 0U));
  constexpr unsigned defined_overflow = 0xf3;
  return {.registers = wanted, .defined_ccr = overflow ? defined_overflow : byte_mask};
}

inline Expected expected(const Input point) {
  auto result = point.before.x == 0 ? zero(point) : finite(point);
  result.registers.pc =
      static_cast<std::uint16_t>(point.before.pc + encode(point.operation).length);
  return result;
}
}
