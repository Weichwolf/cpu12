// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <cstdint>
#include <stdexcept>

namespace cpu12::test {
constexpr unsigned values = 256;
constexpr unsigned byte_mask = 0xff;
constexpr unsigned sign_bit = 0x80;
constexpr unsigned carry = 1;
constexpr unsigned overflow = 2;
constexpr unsigned zero = 4;
constexpr unsigned negative = 8;
constexpr unsigned half = 0x20;
constexpr unsigned nzv = negative | zero | overflow;
constexpr unsigned nzvc = nzv | carry;
constexpr unsigned arithmetic_flags = nzvc | half;
constexpr unsigned accumulator_mask = 0xbf;
constexpr unsigned other_a = 0xa500;
constexpr unsigned other_b = 0x5a;
constexpr unsigned high_mask = 0xff00;
constexpr cpu12::Registers seed{.pc = 0xfffe, .d = 0x3210, .x = 0x1234, .y = 0x5678, .sp = 0x2000};

enum OracleOpcode : std::uint8_t {
  load_a = 0x86,
  add_a = 0x8b,
  sub_a = 0x80,
  cmp_a = 0x81,
  and_a = 0x84,
  or_a = 0x8a,
  xor_a = 0x88,
  test_a = 0x85
};

inline int signed_byte(const unsigned value) {
  return value < sign_bit ? static_cast<int>(value)
                          : static_cast<int>(value) - static_cast<int>(values);
}

struct Input {
  unsigned opcode, left, right, ccr;
};

struct Expected {
  unsigned value, ccr;
  bool store = true;
};

inline Expected expected(const Input input) {
  const auto opcode = input.opcode & accumulator_mask;
  const auto left = input.left;
  const auto right = input.right;
  auto value = right;
  unsigned changed = nzv;
  unsigned flags = 0;
  bool store = true;
  switch (opcode) {
    case load_a: break;
    case add_a: {
      const auto sum = left + right;
      value = sum & byte_mask;
      const auto signed_sum = signed_byte(left) + signed_byte(right);
      flags = (sum >= values ? carry : 0U) |
              (signed_sum < -static_cast<int>(sign_bit) || signed_sum >= static_cast<int>(sign_bit)
                   ? overflow
                   : 0U) |
              ((left % 16 + right % 16) >= 16 ? half : 0U);
      changed = arithmetic_flags;
      break;
    }
    case sub_a:
    case cmp_a: {
      const auto difference = static_cast<int>(left) - static_cast<int>(right);
      value = static_cast<unsigned>(difference + static_cast<int>(values)) % values;
      const auto signed_difference = signed_byte(left) - signed_byte(right);
      flags =
          (difference < 0 ? carry : 0U) | (signed_difference < -static_cast<int>(sign_bit) ||
                                                   signed_difference >= static_cast<int>(sign_bit)
                                               ? overflow
                                               : 0U);
      changed = nzvc;
      store = opcode != cmp_a;
      break;
    }
    case and_a: value = left & right; break;
    case or_a: value = left | right; break;
    case xor_a: value = left ^ right; break;
    case test_a:
      value = left & right;
      store = false;
      break;
    default: throw std::runtime_error("Unsupported test oracle operation");
  }
  flags |= (value == 0 ? zero : 0U) | (value >= sign_bit ? negative : 0U);
  return {.value = value, .ccr = (input.ccr & (byte_mask ^ changed)) | flags, .store = store};
}

}
