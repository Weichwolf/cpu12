// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cpu12::test::indexed {
using Field = std::uint16_t Registers::*;
enum class Offset : std::uint8_t { constant, a, b, d };

struct Form {
  Field base;
  Offset offset = Offset::constant;
  int displacement = 0;
  std::uint8_t postbyte = 0;
  std::size_t extensions = 0;
  bool indirect = false;
};

struct Encoding {
  std::array<std::uint8_t, 4> bytes{};
  std::size_t length = 0;

  [[nodiscard]] std::span<const std::uint8_t> view() const {
    return std::span(bytes).first(length);
  }
};

inline Encoding encode(const Form form, const std::uint8_t opcode) {
  const auto displacement = static_cast<std::uint16_t>(form.displacement);
  constexpr unsigned byte_bits = 8;
  const auto high = static_cast<std::uint8_t>(displacement >> byte_bits);
  const auto low = static_cast<std::uint8_t>(displacement);
  return {.bytes = {opcode, form.postbyte, form.extensions == 1 ? low : high, low},
          .length = 2 + form.extensions};
}

inline std::uint16_t pointer(const Form form, const Registers &before) {
  int displacement = form.displacement;
  constexpr unsigned byte_bits = 8;
  if (form.offset == Offset::a) { displacement = before.d >> byte_bits; }
  constexpr unsigned byte_mask = 0xff;
  if (form.offset == Offset::b) { displacement = static_cast<int>(before.d & byte_mask); }
  if (form.offset == Offset::d) { displacement = before.d; }
  const auto base = before.*form.base;
  const auto pc_adjustment = form.base == &Registers::pc ? 2 + form.extensions : 0;
  return static_cast<std::uint16_t>(static_cast<int>(base) + displacement +
                                    static_cast<int>(pc_adjustment));
}

inline std::vector<Form> forms() {
  struct Base {
    Field field;
    unsigned small, other;
  };

  constexpr std::array bases{Base{.field = &Registers::x, .small = 0x00, .other = 0xe0},
                             Base{.field = &Registers::y, .small = 0x40, .other = 0xe8},
                             Base{.field = &Registers::sp, .small = 0x80, .other = 0xf0},
                             Base{.field = &Registers::pc, .small = 0xc0, .other = 0xf8}};
  constexpr unsigned small_mask = 31;
  std::vector<Form> result;
  for (const auto base : bases) {
    for (int offset = -16; offset <= 15; ++offset) {
      result.push_back({.base = base.field,
                        .displacement = offset,
                        .postbyte = static_cast<std::uint8_t>(
                            base.small | (static_cast<unsigned>(offset) & small_mask))});
    }
    for (const unsigned low : {0U, 1U, 127U, 128U, 255U}) {
      for (const unsigned sign : {0U, 1U}) {
        result.push_back({.base = base.field,
                          .displacement = static_cast<int>(low) - (sign != 0 ? 256 : 0),
                          .postbyte = static_cast<std::uint8_t>(base.other + sign),
                          .extensions = 1});
      }
    }
    for (const int offset : {0, 1, 255, 32767, 32768, 65535}) {
      for (const bool indirect : {false, true}) {
        result.push_back({.base = base.field,
                          .displacement = offset,
                          .postbyte = static_cast<std::uint8_t>(base.other + (indirect ? 3 : 2)),
                          .extensions = 2,
                          .indirect = indirect});
      }
    }
    constexpr std::array offsets{Offset::a, Offset::b, Offset::d, Offset::d};
    for (unsigned mode = 0; mode < offsets.size(); ++mode) {
      result.push_back({.base = base.field,
                        .offset = offsets[mode],
                        .postbyte = static_cast<std::uint8_t>(base.other + 4 + mode),
                        .indirect = mode == 3});
    }
  }
  return result;
}

enum class Function : std::uint8_t { load, add, subtract, compare, store };

struct WordOperation {
  std::uint8_t opcode;
  Field field;
  Function function;
};

inline constexpr std::array words{
    WordOperation{.opcode = 0xec, .field = &Registers::d, .function = Function::load},
    WordOperation{.opcode = 0xed, .field = &Registers::y, .function = Function::load},
    WordOperation{.opcode = 0xee, .field = &Registers::x, .function = Function::load},
    WordOperation{.opcode = 0xef, .field = &Registers::sp, .function = Function::load},
    WordOperation{.opcode = 0xac, .field = &Registers::d, .function = Function::compare},
    WordOperation{.opcode = 0xad, .field = &Registers::y, .function = Function::compare},
    WordOperation{.opcode = 0xae, .field = &Registers::x, .function = Function::compare},
    WordOperation{.opcode = 0xaf, .field = &Registers::sp, .function = Function::compare},
    WordOperation{.opcode = 0xe3, .field = &Registers::d, .function = Function::add},
    WordOperation{.opcode = 0xa3, .field = &Registers::d, .function = Function::subtract},
    WordOperation{.opcode = 0x6c, .field = &Registers::d, .function = Function::store},
    WordOperation{.opcode = 0x6d, .field = &Registers::y, .function = Function::store},
    WordOperation{.opcode = 0x6e, .field = &Registers::x, .function = Function::store},
    WordOperation{.opcode = 0x6f, .field = &Registers::sp, .function = Function::store}};

inline Registers
word_expected(const Registers before, const WordOperation operation, const unsigned operand) {
  auto wanted = before;
  const unsigned left = before.*operation.field;
  constexpr unsigned mask = 0xffff;
  constexpr unsigned sign = 0x8000;
  unsigned result = operand;
  bool carry = false;
  bool overflow = false;
  const bool arithmetic = operation.function == Function::add ||
                          operation.function == Function::subtract ||
                          operation.function == Function::compare;
  if (operation.function == Function::add) {
    result = (left + operand) & mask;
    carry = left + operand > mask;
    overflow = ((left ^ result) & (operand ^ result) & sign) != 0;
  } else if (operation.function == Function::subtract || operation.function == Function::compare) {
    result = (left + mask + 1U - operand) & mask;
    carry = left < operand;
    overflow = ((left ^ operand) & (left ^ result) & sign) != 0;
  } else if (operation.function == Function::store) {
    result = left;
  }
  const unsigned unchanged = arithmetic ? 0xf0 : 0xf1;
  wanted.ccr = static_cast<std::uint8_t>(
      (before.ccr & unchanged) | ((result & sign) != 0 ? 8U : 0U) | (result == 0 ? 4U : 0U) |
      (overflow ? 2U : 0U) | (arithmetic && carry ? 1U : 0U));
  if (operation.function != Function::compare && operation.function != Function::store) {
    wanted.*operation.field = static_cast<std::uint16_t>(result);
  }
  return wanted;
}
}
