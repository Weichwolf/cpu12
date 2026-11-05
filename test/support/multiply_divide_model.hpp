// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::math {
using Byte = std::uint8_t;
constexpr unsigned byte_mask = 0xff, word_mask = 0xffff, word_bits = 16;
constexpr std::uint64_t long_mask = 0xffffffff;
constexpr Registers seed{.pc = 0xfffe, .d = 0, .x = 0xa55a, .y = 0, .sp = 0xffff, .ccr = 0xff};
inline std::uint64_t checked = 0, policy_cases = 0;
enum class Operation : Byte { mul, emul, emuls, idiv, idivs, ediv, edivs, fdiv };

struct Encoding {
  std::array<Byte, 2> bytes;
  unsigned length;

  [[nodiscard]] std::span<const Byte> view() const { return std::span(bytes).first(length); }
};

inline Encoding encode(const Operation operation) {
  constexpr std::array<Encoding, 8> codes{{{.bytes = {0x12, 0}, .length = 1},
                                           {.bytes = {0x13, 0}, .length = 1},
                                           {.bytes = {0x18, 0x13}, .length = 2},
                                           {.bytes = {0x18, 0x10}, .length = 2},
                                           {.bytes = {0x18, 0x15}, .length = 2},
                                           {.bytes = {0x11, 0}, .length = 1},
                                           {.bytes = {0x18, 0x14}, .length = 2},
                                           {.bytes = {0x18, 0x11}, .length = 2}}};
  return codes.at(static_cast<unsigned>(operation));
}

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline std::int64_t signed_word(const unsigned data) {
  constexpr unsigned sign = 0x8000;
  return data < sign ? static_cast<std::int64_t>(data)
                     : static_cast<std::int64_t>(data) - (word_mask + 1U);
}

struct Input {
  Operation operation;
  Registers before;
};

struct Expected {
  Registers registers;
  unsigned defined_ccr = byte_mask;
  bool undefined_d = false;
};

inline Expected multiply(const Input input) {
  auto wanted = input.before;
  const auto d = input.before.d;
  const auto y = input.before.y;
  if (input.operation == Operation::mul) {
    constexpr unsigned byte_range = byte_mask + 1U;
    const auto product = (d / byte_range) * (d % byte_range);
    wanted.d = static_cast<std::uint16_t>(product);
    constexpr unsigned carry_divisor = 0x80;
    wanted.ccr =
        static_cast<Byte>((input.before.ccr & (byte_mask ^ 1U)) | ((product / carry_divisor) % 2U));
  } else {
    const auto product =
        input.operation == Operation::emuls
            ? static_cast<std::uint64_t>(signed_word(d) * signed_word(y)) & long_mask
            : static_cast<std::uint64_t>(d) * y;
    wanted.d = static_cast<std::uint16_t>(product & word_mask);
    wanted.y = static_cast<std::uint16_t>(product / (word_mask + 1U));
    constexpr unsigned retained = 0xf2;
    constexpr unsigned carry_divisor = 0x8000;
    constexpr std::uint64_t sign = 0x80000000;
    wanted.ccr = static_cast<Byte>((input.before.ccr & retained) | (product >= sign ? 8U : 0U) |
                                   (product == 0 ? 4U : 0U) | ((product / carry_divisor) % 2U));
  }
  wanted.pc = static_cast<std::uint16_t>(input.before.pc + encode(input.operation).length);
  return {.registers = wanted};
}

inline void retired(Core &core, const Input input, const Expected expected) {
  core.set_registers(input.before);
  const auto code = encode(input.operation);
  const auto result = core.run_instruction(code.view());
  const auto actual = core.registers();
  auto defined = actual;
  if (expected.undefined_d) { defined.d = expected.registers.d; }
  defined.ccr = expected.registers.ccr;
  require(result.retired == 1 && result.fault == Fault::none && result.pc == input.before.pc &&
              result.opcode == code.bytes[0] && defined == expected.registers &&
              (actual.ccr & expected.defined_ccr) ==
                  (expected.registers.ccr & expected.defined_ccr),
          "Native multiply/divide must match independently specified results and defined flags "
          "without host exception");
  require(actual == expected.registers,
          "Architecturally indeterminate outputs follow the documented core policy, independently "
          "of silicon-equivalence claims");
  ++checked;
  if (expected.undefined_d || expected.defined_ccr != byte_mask) { ++policy_cases; }
}

void divisions(Core &core);
}
