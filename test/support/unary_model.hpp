// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <stdexcept>

namespace cpu12::test::unary {
constexpr unsigned n_bit = 8, z_bit = 4, v_bit = 2, c_bit = 1;
constexpr unsigned mask_byte = 0xff;

enum class Function : std::uint8_t {
  negate,
  complement,
  increment,
  decrement,
  right,
  rotate_left,
  rotate_right,
  signed_right,
  left,
  clear,
  test
};

struct Value {
  unsigned data, flags;
  bool store = true;
};

struct UnaryInput {
  Function function;
  unsigned data, ccr;
  bool word;
};

inline Value expected(const UnaryInput input) {
  constexpr unsigned byte_sign = 0x80;
  constexpr unsigned word_sign = 0x8000;
  const unsigned sign = input.word ? word_sign : byte_sign;
  const auto range = sign * 2U;
  const auto mask = range - 1U;
  const auto source = input.data;
  auto result = source;
  unsigned changed = n_bit | z_bit | v_bit | c_bit;
  bool c = false;
  bool v = false;
  bool store = true;
  switch (input.function) {
    case Function::negate:
      result = (range - source) & mask;
      c = source != 0;
      v = source == sign;
      break;
    case Function::complement:
      result = mask - source;
      c = true;
      break;
    case Function::increment:
      result = (source + 1U) & mask;
      v = source == sign - 1U;
      changed ^= c_bit;
      break;
    case Function::decrement:
      result = (source + mask) & mask;
      v = source == sign;
      changed ^= c_bit;
      break;
    case Function::right:
      result = source / 2U;
      c = source % 2U != 0;
      break;
    case Function::rotate_left:
      result = ((source * 2U) + (input.ccr & c_bit)) & mask;
      c = source >= sign;
      break;
    case Function::rotate_right:
      result = (source / 2U) + ((input.ccr & c_bit) != 0 ? sign : 0U);
      c = source % 2U != 0;
      break;
    case Function::signed_right:
      result = (source / 2U) + (source >= sign ? sign : 0U);
      c = source % 2U != 0;
      break;
    case Function::left:
      result = (source * 2U) & mask;
      c = source >= sign;
      break;
    case Function::clear: result = 0; break;
    case Function::test: store = false; break;
    default: throw std::runtime_error("Unsupported independently authored unary expectation");
  }
  if (input.function >= Function::right && input.function <= Function::left) {
    v = (result >= sign) != c;
  }
  const unsigned flags = (result >= sign ? n_bit : 0U) | (result == 0 ? z_bit : 0U) |
                         (v ? v_bit : 0U) | (c ? c_bit : 0U);
  return {.data = result, .flags = (input.ccr & (mask_byte ^ changed)) | flags, .store = store};
}

}
