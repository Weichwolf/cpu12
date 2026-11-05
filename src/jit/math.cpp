// SPDX-License-Identifier: GPL-3.0-only
#include "jit/math.hpp"

#include "cpu12/core.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace cpu12::detail {
namespace {
constexpr auto d = static_cast<std::uint8_t>(offsetof(Registers, d));
constexpr auto x = static_cast<std::uint8_t>(offsetof(Registers, x));
constexpr auto y = static_cast<std::uint8_t>(offsetof(Registers, y));
constexpr auto ccr = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr std::array<std::uint8_t, 4> load_d{0x0f, 0xb7, 0x47, d};
constexpr std::array<std::uint8_t, 4> load_y{0x0f, 0xb7, 0x57, y};
constexpr std::array<std::uint8_t, 4> load_x{0x0f, 0xb7, 0x4f, x};
constexpr std::array<std::uint8_t, 4> signed_d{0x0f, 0xbf, 0x47, d};
constexpr std::array<std::uint8_t, 4> signed_x{0x0f, 0xbf, 0x4f, x};
constexpr std::array<std::uint8_t, 5> signed_x64{0x48, 0x0f, 0xbf, 0x4f, x};
constexpr std::array<std::uint8_t, 4> store_remainder{0x66, 0x89, 0x57, d};
constexpr std::array<std::uint8_t, 3> store_quotient{0x66, 0x89, 0x47};
constexpr std::array<std::uint8_t, 2> jump_zero{0x0f, 0x84};
constexpr std::array<std::uint8_t, 2> compare{0x39, 0xc8};
constexpr std::array<std::uint8_t, 8> place_nz{0xc0, 0xe1, 0x03, 0xc0, 0xe2, 0x02, 0x08, 0xd1};

bool extended(const MathOp operation) {
  return operation == MathOp::divide_extended || operation == MathOp::divide_extended_signed;
}

void finish(Code &code, const Instruction &instruction) {
  code.advance(instruction.length);
  code.return_();
}

void store(Code &code, const MathOp operation) {
  code.raw(store_quotient);
  code.byte(extended(operation) ? y : x);
  code.raw(store_remainder);
}

void flags(Code &code, const MathOp operation) {
  const bool nz = extended(operation) || operation == MathOp::divide_signed;
  constexpr std::array<std::uint8_t, 3> test_word{0x66, 0x85, 0xc0};
  constexpr std::array<std::uint8_t, 3> zero{0x0f, 0x94, 0xc1};
  constexpr std::array<std::uint8_t, 6> result_nz{0x0f, 0x98, 0xc1, 0x0f, 0x94, 0xc2};
  code.raw(test_word);
  if (nz) {
    code.raw(result_nz);
    code.raw(place_nz);
  } else {
    code.raw(zero);
    constexpr std::array<std::uint8_t, 3> place_zero{0xc0, 0xe1, 0x02};
    code.raw(place_zero);
  }
  if (operation == MathOp::divide_signed) {
    constexpr std::array<std::uint8_t, 8> overflow{0x41, 0x0f, 0xb6, 0xd0, 0xd0, 0xe2, 0x08, 0xd1};
    code.raw(overflow);
  }
  constexpr std::array<std::uint8_t, 3> retain_other{0x80, 0x67, ccr};
  constexpr std::uint8_t retain_upper = 0xf0;
  constexpr std::uint8_t retain_negative = 0xf8;
  code.raw(retain_other);
  code.byte(nz ? retain_upper : retain_negative);
  constexpr std::array<std::uint8_t, 3> merge{0x08, 0x4f, ccr};
  code.raw(merge);
}

void dividend(Code &code, const MathOp operation) {
  if (operation == MathOp::divide_signed) {
    code.raw(signed_d);
    constexpr std::uint8_t cdq = 0x99;
    code.byte(cdq);
    return;
  }
  code.raw(load_d);
  if (extended(operation)) {
    code.raw(load_y);
    constexpr std::array<std::uint8_t, 5> upper{0xc1, 0xe2, 0x10, 0x09, 0xd0};
    code.raw(upper);
  } else if (operation == MathOp::divide_fractional) {
    constexpr std::array<std::uint8_t, 3> fraction{0xc1, 0xe0, 0x10};
    code.raw(fraction);
  }
  if (operation == MathOp::divide_extended_signed) {
    constexpr std::array<std::uint8_t, 5> extend{0x48, 0x63, 0xc0, 0x48, 0x99};
    code.raw(extend);
  } else {
    constexpr std::array<std::uint8_t, 2> clear_high{0x31, 0xd2};
    code.raw(clear_high);
  }
}

std::optional<std::size_t> overflow_guard(Code &code, const MathOp operation) {
  if (operation != MathOp::divide_extended && operation != MathOp::divide_fractional) { return {}; }
  if (operation == MathOp::divide_extended) {
    code.raw(load_y);
  } else {
    code.raw(load_d);
  }
  if (operation == MathOp::divide_extended) {
    constexpr std::array<std::uint8_t, 2> high_eax{0x89, 0xd0};
    code.raw(high_eax);
  }
  code.raw(compare);
  constexpr std::array<std::uint8_t, 2> above_equal{0x0f, 0x83};
  code.raw(above_equal);
  return code.relative32();
}

void overflow_case(Code &code, const MathOp operation) {
  if (operation == MathOp::divide_fractional) {
    constexpr std::array<std::uint8_t, 6> saturated{0x66, 0xc7, 0x47, x, 0xff, 0xff};
    constexpr std::array<std::uint8_t, 4> retain_other{0x80, 0x67, ccr, 0xf8};
    code.raw(saturated);
    code.raw(retain_other);
  } else {
    constexpr std::array<std::uint8_t, 4> clear_carry{0x80, 0x67, ccr, 0xfe};
    code.raw(clear_carry);
  }
  constexpr std::array<std::uint8_t, 4> overflow{0x80, 0x4f, ccr, 0x02};
  code.raw(overflow);
}

void zero_case(Code &code, const MathOp operation) {
  if (operation == MathOp::divide_unsigned || operation == MathOp::divide_fractional) {
    constexpr std::array<std::uint8_t, 6> saturated{0x66, 0xc7, 0x47, x, 0xff, 0xff};
    constexpr std::array<std::uint8_t, 4> retain_other{0x80, 0x67, ccr, 0xf8};
    code.raw(saturated);
    code.raw(retain_other);
  }
  constexpr std::array<std::uint8_t, 3> set_flags{0x80, 0x4f, ccr};
  code.raw(set_flags);
  code.byte(operation == MathOp::divide_fractional ? 3 : 1);
}

constexpr std::array<std::uint8_t, 3> store_d{0x66, 0x89, 0x47};
constexpr std::array<std::uint8_t, 3> multiply{0x0f, 0xaf, 0xc1};

void byte_multiply(Code &code) {
  constexpr std::array<std::uint8_t, 4> load_a{0x0f, 0xb6, 0x47, d + 1};
  constexpr std::array<std::uint8_t, 4> load_b{0x0f, 0xb6, 0x4f, d};
  constexpr std::array<std::uint8_t, 3> bit_seven{0xc1, 0xe8, 0x07};
  constexpr std::array<std::uint8_t, 2> mask_carry{0x24, 0x01};
  constexpr std::array<std::uint8_t, 4> retain_other{0x80, 0x67, ccr, 0xfe};
  constexpr std::array<std::uint8_t, 3> merge_carry{0x08, 0x47, ccr};
  code.raw(load_a);
  code.raw(load_b);
  code.raw(multiply);
  code.raw(store_d);
  code.byte(d);
  if (!code.flags_live()) { return; }
  code.raw(bit_seven);
  code.raw(mask_carry);
  code.raw(retain_other);
  code.raw(merge_carry);
}

void extended_multiply(Code &code, const MathOp operation) {
  constexpr std::array<std::uint8_t, 2> load{0x0f, 0xb7};
  constexpr std::array<std::uint8_t, 2> load_signed{0x0f, 0xbf};
  code.raw(operation == MathOp::multiply_signed ? load_signed : load);
  constexpr std::uint8_t eax_d = 0x47;
  constexpr std::uint8_t ecx_y = 0x4f;
  code.byte(eax_d);
  code.byte(d);
  code.raw(operation == MathOp::multiply_signed ? load_signed : load);
  code.byte(ecx_y);
  code.byte(y);
  code.raw(multiply);
  code.raw(store_d);
  code.byte(d);
  constexpr std::array<std::uint8_t, 5> high_word{0x89, 0xc2, 0xc1, 0xea, 0x10};
  constexpr std::array<std::uint8_t, 4> store_y{0x66, 0x89, 0x57, y};
  code.raw(high_word);
  code.raw(store_y);
  constexpr std::array<std::uint8_t, 8> nz{0x85, 0xc0, 0x0f, 0x98, 0xc1, 0x0f, 0x94, 0xc2};
  constexpr std::array<std::uint8_t, 7> carry{0xc1, 0xe8, 0x0f, 0x24, 0x01, 0x08, 0xc1};
  constexpr std::array<std::uint8_t, 4> retain_other{0x80, 0x67, ccr, 0xf2};
  constexpr std::array<std::uint8_t, 3> merge{0x08, 0x4f, ccr};
  if (!code.flags_live()) { return; }
  code.raw(nz);
  code.raw(place_nz);
  code.raw(carry);
  code.raw(retain_other);
  code.raw(merge);
}

void divide_instruction(Code &code, const Instruction &instruction) {
  const auto operation = instruction.math;
  if (operation == MathOp::divide_extended_signed) {
    code.raw(signed_x64);
  } else if (operation == MathOp::divide_signed) {
    code.raw(signed_x);
  } else {
    code.raw(load_x);
  }
  constexpr std::array<std::uint8_t, 2> test_divisor{0x85, 0xc9};
  code.raw(test_divisor);
  code.raw(jump_zero);
  const auto zero = code.relative32();
  auto overflow = overflow_guard(code, operation);
  dividend(code, operation);
  if (operation == MathOp::divide_extended_signed) {
    constexpr std::array<std::uint8_t, 3> divide{0x48, 0xf7, 0xf9};
    code.raw(divide);
    constexpr std::array<std::uint8_t, 16> range{0x44,
                                                 0x8d,
                                                 0x80,
                                                 0x00,
                                                 0x80,
                                                 0x00,
                                                 0x00,
                                                 0x41,
                                                 0x81,
                                                 0xf8,
                                                 0xff,
                                                 0xff,
                                                 0x00,
                                                 0x00,
                                                 0x0f,
                                                 0x87};
    code.raw(range);
    overflow = code.relative32();
  } else {
    constexpr std::array<std::uint8_t, 2> unsigned_divide{0xf7, 0xf1};
    constexpr std::array<std::uint8_t, 2> signed_divide{0xf7, 0xf9};
    code.raw(operation == MathOp::divide_signed ? signed_divide : unsigned_divide);
    if (operation == MathOp::divide_signed) {
      constexpr std::array<std::uint8_t, 9> out_of_range{
          0x3d, 0x00, 0x80, 0x00, 0x00, 0x41, 0x0f, 0x94, 0xc0};
      code.raw(out_of_range);
    }
  }
  store(code, operation);
  flags(code, operation);
  finish(code, instruction);
  if (overflow) {
    code.bind(*overflow);
    overflow_case(code, operation);
    finish(code, instruction);
  }
  code.bind(zero);
  zero_case(code, operation);
  finish(code, instruction);
}
}

void math_instruction(Code &code, const Instruction &instruction) {
  switch (instruction.math) {
    case MathOp::multiply_byte: byte_multiply(code); break;
    case MathOp::multiply_unsigned:
    case MathOp::multiply_signed: extended_multiply(code, instruction.math); break;
    case MathOp::divide_unsigned:
    case MathOp::divide_signed:
    case MathOp::divide_extended:
    case MathOp::divide_extended_signed:
    case MathOp::divide_fractional: divide_instruction(code, instruction); return;
    default: throw std::logic_error("Unknown native multiply/divide operation");
  }
  code.advance(instruction.length);
  code.return_();
}
}
