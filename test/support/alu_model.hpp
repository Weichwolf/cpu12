// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::alu {
using Byte = std::uint8_t;
constexpr unsigned byte_bits = 8;
constexpr unsigned byte_mask = 0xff;
constexpr unsigned high_mask = 0xff00;
constexpr unsigned negative = 8, zero = 4, overflow = 2, carry = 1, half = 0x20;
constexpr Registers seed{
    .pc = 0xffff, .d = 0xa55a, .x = 0x1234, .y = 0xabcd, .sp = 0x8000, .ccr = 0xff};
inline std::uint64_t checked = 0;

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline Registers accumulator(Registers before, const bool b, const unsigned value) {
  before.d = static_cast<std::uint16_t>(b ? (before.d & high_mask) | value
                                          : (before.d & byte_mask) | (value << byte_bits));
  return before;
}

inline void retired(Core &core,
                    const std::span<const Byte> code,
                    const Registers before,
                    const Registers wanted) {
  core.set_registers(before);
  const auto actual = core.run_instruction(code);
  if (actual.retired != 1 || actual.fault != Fault::none || actual.pc != before.pc ||
      actual.opcode != code[0] || core.registers() != wanted) {
    const auto result = core.registers();
    throw std::runtime_error(
        "ALU opcode=" + std::to_string(code[0]) + " oldD=" + std::to_string(before.d) +
        " oldCCR=" + std::to_string(before.ccr) + " wantedD=" + std::to_string(wanted.d) +
        " actualD=" + std::to_string(result.d) + " wantedCCR=" + std::to_string(wanted.ccr) +
        " actualCCR=" + std::to_string(result.ccr));
  }
  ++checked;
}

void carry_operations();
}
