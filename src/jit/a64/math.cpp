// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"

#include <stdexcept>

namespace cpu12::detail::a64 {
void Emitter::divide(const MathOp operation) {
  const bool extended =
      operation == MathOp::divide_extended || operation == MathOp::divide_extended_signed;
  const bool fractional = operation == MathOp::divide_fractional;
  const bool sign =
      operation == MathOp::divide_signed || operation == MathOp::divide_extended_signed;
  const auto width = operation == MathOp::divide_extended_signed ? Width::x64 : Width::w32;
  Label zero;
  Label overflow;
  Label complete;
  code_.extract(Reg::x1, x, 0, word_bits, width, sign);
  code_.branch_zero_far(Reg::x1, zero, width);
  code_.move(Reg::x0, d, Width::w32);
  if (extended) {
    code_.shift(Shift::left, Reg::x5, y, word_bits);
    code_.alu(Alu::bit_or, Reg::x0, Reg::x0, Reg::x5);
  } else if (fractional) {
    code_.shift(Shift::left, Reg::x0, Reg::x0, word_bits);
  }
  if (sign) {
    code_.extract(Reg::x0, Reg::x0, 0, extended ? 2 * word_bits : word_bits, width, true);
  }
  if (!sign && (extended || fractional)) {
    code_.compare(extended ? y : d, Reg::x1);
    code_.branch_far(Condition::carry, overflow);
  }
  code_.divide(Reg::x2, Reg::x0, Reg::x1, sign, width);
  code_.multiply_subtract(Reg::x8, Reg::x2, Reg::x1, Reg::x0, width);
  if (operation == MathOp::divide_extended_signed) {
    constexpr unsigned signed_bias = 0x8000;
    code_.constant(Reg::x17, signed_bias);
    code_.alu(Alu::add, Reg::x5, Reg::x2, Reg::x17, Width::x64);
    code_.constant(Reg::x17, word_mask);
    code_.compare(Reg::x5, Reg::x17, Width::x64);
    code_.branch_far(Condition::above, overflow);
  }
  if (operation == MathOp::divide_signed) {
    constexpr unsigned overflowing_quotient = 0x8000;
    code_.constant(Reg::x17, overflowing_quotient);
    code_.compare(Reg::x2, Reg::x17);
    code_.set(Reg::x7, Condition::equal);
    code_.shift(Shift::left, Reg::x7, Reg::x7, 1);
  } else {
    code_.constant(Reg::x7, 0);
  }
  code_.extract(Reg::x2, Reg::x2, 0, word_bits);
  code_.move(extended ? y : x, Reg::x2, Width::w32);
  code_.extract(d, Reg::x8, 0, word_bits);
  flags(word_bits, {.changed = flag_z | flag_v | flag_c | (extended || sign ? flag_n : 0U)});
  code_.alu(Alu::bit_or, ccr, Reg::x4, Reg::x7);
  code_.branch(complete);
  code_.bind(overflow);
  if (fractional) {
    code_.constant(x, word_mask);
    mask(ccr, ccr, byte_mask ^ (flag_z | flag_v | flag_c));
  } else {
    mask(ccr, ccr, byte_mask ^ flag_c);
  }
  code_.constant(Reg::x17, flag_v);
  code_.alu(Alu::bit_or, ccr, ccr, Reg::x17);
  code_.branch(complete);
  code_.bind(zero);
  if (fractional || operation == MathOp::divide_unsigned) {
    code_.constant(x, word_mask);
    mask(ccr, ccr, byte_mask ^ (flag_z | flag_v | flag_c));
  }
  code_.constant(Reg::x17, flag_c | (fractional ? flag_v : 0U));
  code_.alu(Alu::bit_or, ccr, ccr, Reg::x17);
  code_.bind(complete);
}

void Emitter::math(const Instruction &operation) {
  const auto math = operation.math;
  switch (math) {
    case MathOp::multiply_byte:
      code_.extract(Reg::x0, d, byte_bits, byte_bits);
      code_.extract(Reg::x1, d, 0, byte_bits);
      code_.multiply(d, Reg::x0, Reg::x1);
      mask(ccr, ccr, byte_mask ^ flag_c);
      code_.extract(Reg::x5, d, byte_bits - 1, 1);
      code_.alu(Alu::bit_or, ccr, ccr, Reg::x5);
      return;
    case MathOp::multiply_unsigned:
    case MathOp::multiply_signed:
      code_.extract(Reg::x0, d, 0, word_bits, Width::w32, math == MathOp::multiply_signed);
      code_.extract(Reg::x1, y, 0, word_bits, Width::w32, math == MathOp::multiply_signed);
      code_.multiply(Reg::x2, Reg::x0, Reg::x1);
      code_.extract(d, Reg::x2, 0, word_bits);
      code_.extract(y, Reg::x2, word_bits, word_bits);
      mask(ccr, ccr, byte_mask ^ (flag_n | flag_z | flag_c));
      code_.extract(Reg::x5, Reg::x2, (2 * word_bits) - 1, 1);
      code_.shift(Shift::left, Reg::x5, Reg::x5, 3);
      code_.alu(Alu::bit_or, ccr, ccr, Reg::x5);
      code_.compare(Reg::x2, Reg::zr);
      code_.set(Reg::x5, Condition::equal);
      code_.shift(Shift::left, Reg::x5, Reg::x5, 2);
      code_.alu(Alu::bit_or, ccr, ccr, Reg::x5);
      code_.extract(Reg::x5, Reg::x2, word_bits - 1, 1);
      code_.alu(Alu::bit_or, ccr, ccr, Reg::x5);
      return;
    case MathOp::divide_unsigned:
    case MathOp::divide_signed:
    case MathOp::divide_extended:
    case MathOp::divide_extended_signed:
    case MathOp::divide_fractional: divide(math); return;
    default: throw std::logic_error("Unknown CPU12 A64 multiply/divide instruction");
  }
}
}
