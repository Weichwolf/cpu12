// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "observed_core.hpp"
#include "unary_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::memory_unary {
using Byte = std::uint8_t;
constexpr unsigned byte_mask = 0xff;
constexpr unsigned byte_bits = 8;
constexpr std::size_t memory_bytes = 65536;
constexpr Registers seed{.pc = 0xfffe, .d = 0x80ff, .x = 0, .y = 0x7fff, .sp = 0xffff, .ccr = 0xff};
inline std::uint64_t checked = 0;

struct Operation {
  Byte indexed, extended;
  unary::Function function;
  bool read = true, write = true;
};

constexpr std::array<Operation, 11> operations{
    {{.indexed = 0x60, .extended = 0x70, .function = unary::Function::negate},
     {.indexed = 0x61, .extended = 0x71, .function = unary::Function::complement},
     {.indexed = 0x62, .extended = 0x72, .function = unary::Function::increment},
     {.indexed = 0x63, .extended = 0x73, .function = unary::Function::decrement},
     {.indexed = 0x64, .extended = 0x74, .function = unary::Function::right},
     {.indexed = 0x65, .extended = 0x75, .function = unary::Function::rotate_left},
     {.indexed = 0x66, .extended = 0x76, .function = unary::Function::rotate_right},
     {.indexed = 0x67, .extended = 0x77, .function = unary::Function::signed_right},
     {.indexed = 0x68, .extended = 0x78, .function = unary::Function::left},
     {.indexed = 0x69, .extended = 0x79, .function = unary::Function::clear, .read = false},
     {.indexed = 0xe7, .extended = 0xf7, .function = unary::Function::test, .write = false}}};

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline unary::Value
prediction(const Operation operation, const Registers before, const unsigned data) {
  return unary::expected(
      {.function = operation.function, .data = data, .ccr = before.ccr, .word = false});
}

inline Registers expected(const Registers before,
                          Registers updated,
                          const std::span<const Byte> code,
                          const unary::Value result) {
  updated.ccr = static_cast<Byte>(result.flags);
  updated.pc = static_cast<std::uint16_t>(before.pc + code.size());
  return updated;
}

inline void retired(ObservedCore &core,
                    const std::span<const Byte> code,
                    const Registers before,
                    const Registers wanted) {
  core.set_registers(before);
  const auto result = core.run_instruction(code);
  if (result.retired != 1 || result.fault != Fault::none || result.pc != before.pc ||
      result.opcode != code[0] || core.registers() != wanted) {
    const auto actual = core.registers();
    throw std::runtime_error(
        "Memory unary opcode=" + std::to_string(code[0]) + " oldCCR=" + std::to_string(before.ccr) +
        " wantedCCR=" + std::to_string(wanted.ccr) + " actualCCR=" + std::to_string(actual.ccr) +
        " fault=" + std::to_string(static_cast<unsigned>(result.fault)));
  }
  ++checked;
}

void external_cases();
}
