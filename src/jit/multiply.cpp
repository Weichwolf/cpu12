// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/multiply_divide.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace cpu12::detail {
namespace {
constexpr auto d = static_cast<std::uint8_t>(offsetof(Registers, d));
constexpr auto y = static_cast<std::uint8_t>(offsetof(Registers, y));
constexpr auto ccr = static_cast<std::uint8_t>(offsetof(Registers, ccr));
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
  constexpr std::array<std::uint8_t, 8> place_nz{0xc0, 0xe1, 0x03, 0xc0, 0xe2, 0x02, 0x08, 0xd1};
  constexpr std::array<std::uint8_t, 7> carry{0xc1, 0xe8, 0x0f, 0x24, 0x01, 0x08, 0xc1};
  constexpr std::array<std::uint8_t, 4> retain_other{0x80, 0x67, ccr, 0xf2};
  constexpr std::array<std::uint8_t, 3> merge{0x08, 0x4f, ccr};
  code.raw(nz);
  code.raw(place_nz);
  code.raw(carry);
  code.raw(retain_other);
  code.raw(merge);
}
}

void multiply_divide_instruction(Code &code, const Instruction &instruction) {
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
