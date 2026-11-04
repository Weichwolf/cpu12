// SPDX-License-Identifier: GPL-3.0-only
#include "jit/accumulator.hpp"

#include "cpu12/core.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/flags.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cpu12::detail {
namespace {
constexpr auto a = static_cast<std::uint8_t>(offsetof(Registers, d) + 1);
constexpr auto b = static_cast<std::uint8_t>(offsetof(Registers, d));
constexpr auto ccr = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr unsigned byte_mask = 0xff;
constexpr unsigned carry_index = 0x100;
constexpr unsigned half_index = 0x200;
constexpr unsigned unsupported = 0x8000;

constexpr auto decimal_table() {
  std::array<std::uint16_t, 1024> result{};
  for (unsigned i = 0; i < result.size(); ++i) {
    const auto data = i & byte_mask;
    const auto low = data & 15U;
    const auto high = data >> 4U;
    const bool carry = (i & carry_index) != 0;
    const bool half = (i & half_index) != 0;
    // S12CPUV2's nine rows reduce to these four C/H input domains.
    const bool low_qualified = !half || low <= 3;
    const bool high_qualified = !carry || high <= (half ? 3U : 2U);
    const bool qualified = low_qualified && high_qualified;
    if (!qualified) {
      result[i] = unsupported;
      continue;
    }
    constexpr unsigned upper_limit = 0x99;
    const bool corrected_carry = carry || data > upper_limit;
    const unsigned low_correction = half || low > 9 ? 6 : 0;
    constexpr unsigned upper_correction = 0x60;
    const auto correction = low_correction + (corrected_carry ? upper_correction : 0U);
    result[i] = static_cast<std::uint16_t>(((data + correction) & byte_mask) |
                                           (corrected_carry ? carry_index : 0U));
  }
  return result;
}

constexpr auto decimal = decimal_table();

void invalid_decimal(Code &code) {
  constexpr auto fault = static_cast<std::uint8_t>(offsetof(NativeContext, fault));
  constexpr auto address = static_cast<std::uint8_t>(offsetof(NativeContext, address));
  constexpr auto value = static_cast<std::uint8_t>(offsetof(NativeContext, value));
  constexpr auto width = static_cast<std::uint8_t>(offsetof(NativeContext, access_size));
  static_assert(offsetof(NativeContext, access_size) < 128);
  constexpr std::array<std::uint8_t, 4> actual_input{0x66, 0x89, 0x4e, value};
  constexpr std::array<std::uint8_t, 6> no_address{0x66, 0xc7, 0x46, address, 0, 0};
  constexpr std::array<std::uint8_t, 4> no_width{0xc6, 0x46, width, 0};
  constexpr std::array<std::uint8_t, 4> unqualified{
      0xc6, 0x46, fault, static_cast<std::uint8_t>(Fault::unsupported_state)};
  code.raw(actual_input);
  code.raw(no_address);
  code.raw(no_width);
  code.raw(unqualified);
  code.return_();
}

void decimal_adjust(Code &code, const Instruction &instruction) {
  constexpr std::array<std::uint8_t, 4> load_a{0x0f, 0xb6, 0x47, a};
  constexpr std::array<std::uint8_t, 4> load_ccr{0x0f, 0xb6, 0x4f, ccr};
  constexpr std::array<std::uint8_t, 10> carry{
      0x89, 0xca, 0x83, 0xe2, 0x01, 0xc1, 0xe2, 0x08, 0x09, 0xd0};
  constexpr std::array<std::uint8_t, 8> half{0x83, 0xe1, 0x20, 0xc1, 0xe1, 0x04, 0x09, 0xc8};
  constexpr std::array<std::uint8_t, 2> save_input{0x89, 0xc1};
  constexpr std::array<std::uint8_t, 2> table_pointer{0x49, 0xbb};
  constexpr std::array<std::uint8_t, 5> lookup{0x41, 0x0f, 0xb7, 0x04, 0x43};
  code.raw(load_a);
  code.raw(load_ccr);
  code.raw(carry);
  code.raw(half);
  code.raw(save_input);
  code.raw(table_pointer);
  code.u64(reinterpret_cast<std::uintptr_t>(decimal.data()));
  code.raw(lookup);
  constexpr std::array<std::uint8_t, 5> unqualified{0xa9, 0x00, 0x80, 0x00, 0x00};
  constexpr std::array<std::uint8_t, 2> not_zero{0x0f, 0x85};
  code.raw(unqualified);
  code.raw(not_zero);
  const auto invalid = code.relative32();
  constexpr std::array<std::uint8_t, 3> store_a{0x88, 0x47, a};
  constexpr std::array<std::uint8_t, 5> output_carry{0x89, 0xc1, 0xc1, 0xe9, 0x08};
  constexpr std::array<std::uint8_t, 9> nz{0x84, 0xc0, 0x0f, 0x98, 0xc2, 0x41, 0x0f, 0x94, 0xc0};
  constexpr std::array<std::uint8_t, 5> negative{0xc0, 0xe2, 0x03, 0x08, 0xd1};
  constexpr std::array<std::uint8_t, 7> zero{0x41, 0xc0, 0xe0, 0x02, 0x44, 0x08, 0xc1};
  constexpr std::array<std::uint8_t, 4> preserve{0x80, 0x67, ccr, 0xf2};
  constexpr std::array<std::uint8_t, 3> publish{0x08, 0x4f, ccr};
  code.raw(store_a);
  code.raw(output_carry);
  code.raw(nz);
  code.raw(negative);
  code.raw(zero);
  code.raw(preserve);
  code.raw(publish);
  code.advance(instruction.length);
  code.return_();
  code.bind(invalid);
  invalid_decimal(code);
}

void accumulator_arithmetic(Code &code, const Instruction &instruction) {
  constexpr std::array<std::uint8_t, 4> load_a{0x0f, 0xb6, 0x47, a};
  constexpr std::array<std::uint8_t, 4> load_b{0x0f, 0xb6, 0x4f, b};
  code.raw(load_a);
  code.raw(load_b);
  constexpr std::array<std::uint8_t, 2> add{0x02, 0xc1};
  constexpr std::array<std::uint8_t, 2> subtract{0x2a, 0xc1};
  constexpr std::array<std::uint8_t, 2> compare{0x3a, 0xc1};
  if (instruction.operation == RegisterOp::add_b) {
    code.raw(add);
  } else if (instruction.operation == RegisterOp::subtract_b) {
    code.raw(subtract);
  } else {
    code.raw(compare);
  }
  if (instruction.operation != RegisterOp::compare_b) {
    constexpr std::array<std::uint8_t, 3> store_a{0x88, 0x47, a};
    code.raw(store_a);
  }
  flags(code,
        {.carry = true, .overflow = true, .half = instruction.operation == RegisterOp::add_b});
  code.advance(instruction.length);
  code.return_();
}
}

void accumulator_instruction(Code &code, const Instruction &instruction) {
  if (instruction.operation == RegisterOp::decimal_adjust) {
    decimal_adjust(code, instruction);
  } else {
    accumulator_arithmetic(code, instruction);
  }
}
}
