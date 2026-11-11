// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/math.hpp"

#include "instruction.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/flags.hpp"
#include "jit/llvm/lift.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Types.h"

#include <cstdint>
#include <stdexcept>

namespace cpu12::detail::llvm19 {
namespace {
constexpr unsigned byte_bits = 8, word_bits = 16, byte_mask = 0xff, word_mask = 0xffff;
constexpr unsigned carry = 1, overflow = 2, zero = 4, negative = 8;

struct Shape {
  bool extended, sign, fractional;
};

Shape shape(const MathOp operation) {
  return {.extended =
              operation == MathOp::divide_extended || operation == MathOp::divide_extended_signed,
          .sign = operation == MathOp::divide_signed || operation == MathOp::divide_extended_signed,
          .fractional = operation == MathOp::divide_fractional};
}

LLVMValueRef flag(Builder &b, LLVMValueRef condition, const unsigned mask) {
  return b.CreateSelect(condition, b.getInt32(mask), b.getInt32(0));
}

LLVMValueRef extend(Builder &b, LLVMValueRef value, const bool sign, const unsigned width) {
  if (!sign) { return b.CreateZExt(value, b.getInt64Ty()); }
  auto *narrow = width == word_bits ? b.CreateTrunc(value, b.getInt16Ty()) : value;
  return LLVMBuildSExt(b.get(), narrow, b.getInt64Ty(), "");
}

struct Division {
  LLVMValueRef quotient, remainder, zero, overflow;
};

Division division(Builder &b, const Values &state, const Shape mode) {
  auto *raw = state.d;
  if (mode.extended) { raw = b.CreateOr(b.CreateShl(state.y, b.getInt32(word_bits)), raw); }
  if (mode.fractional) { raw = b.CreateShl(raw, b.getInt32(word_bits)); }
  // MIN32/-1 must remain defined before selecting the guest overflow policy.
  auto *numerator = extend(b, raw, mode.sign, mode.extended ? 2 * word_bits : word_bits);
  auto *divisor = extend(b, state.x, mode.sign, word_bits);
  auto *is_zero = b.CreateICmpEQ(state.x, b.getInt32(0));
  auto *safe = b.CreateSelect(is_zero, b.getInt64(1), divisor);
  auto *quotient = mode.sign ? LLVMBuildSDiv(b.get(), numerator, safe, "")
                             : LLVMBuildUDiv(b.get(), numerator, safe, "");
  auto *remainder = mode.sign ? LLVMBuildSRem(b.get(), numerator, safe, "")
                              : LLVMBuildURem(b.get(), numerator, safe, "");
  LLVMValueRef out = nullptr;
  if (mode.sign) {
    constexpr std::int64_t minimum = -32768;
    constexpr std::uint64_t maximum = 32767;
    out = b.CreateOr(
        LLVMBuildICmp(
            b.get(), LLVMIntSLT, quotient, b.getInt64(static_cast<std::uint64_t>(minimum)), ""),
        LLVMBuildICmp(b.get(), LLVMIntSGT, quotient, b.getInt64(maximum), ""));
  } else {
    out = b.CreateICmpUGT(quotient, b.getInt64(word_mask));
  }
  return {.quotient = quotient, .remainder = remainder, .zero = is_zero, .overflow = out};
}

LLVMValueRef zero_flags(Builder &b, LLVMValueRef ccr, const MathOp operation) {
  if (operation != MathOp::divide_unsigned && operation != MathOp::divide_fractional) {
    return b.CreateOr(ccr, b.getInt32(carry));
  }
  const auto bits = carry | (operation == MathOp::divide_fractional ? overflow : 0U);
  return b.CreateOr(b.CreateAnd(ccr, b.getInt32(byte_mask ^ (zero | overflow | carry))),
                    b.getInt32(bits));
}

void divide(Builder &b, Values &state, const MathOp operation) {
  const auto mode = shape(operation);
  const auto result = division(b, state, mode);
  auto *accepted = b.CreateNot(result.zero);
  if (mode.extended || mode.fractional) {
    accepted = b.CreateAnd(accepted, b.CreateNot(result.overflow));
  }
  auto *quotient =
      b.CreateAnd(b.CreateTrunc(result.quotient, b.getInt32Ty()), b.getInt32(word_mask));
  auto *remainder =
      b.CreateAnd(b.CreateTrunc(result.remainder, b.getInt32Ty()), b.getInt32(word_mask));
  auto *valid = flags(b,
                      state.ccr,
                      {.value = quotient,
                       .carry_bit = b.getInt32(0),
                       .overflow_bit = flag(b, result.overflow, overflow),
                       .width = word_bits});
  if (!mode.sign && !mode.extended) {
    valid = b.CreateOr(b.CreateAnd(valid, b.getInt32(byte_mask ^ negative)),
                       b.CreateAnd(state.ccr, b.getInt32(negative)));
  }
  const auto clear = mode.fractional ? zero | overflow | carry : carry;
  auto *over =
      b.CreateOr(b.CreateAnd(state.ccr, b.getInt32(byte_mask ^ clear)), b.getInt32(overflow));
  auto *ccr = b.CreateSelect(
      result.zero, zero_flags(b, state.ccr, operation), b.CreateSelect(accepted, valid, over));
  state.d = b.CreateSelect(accepted, remainder, state.d);
  if (mode.extended) {
    state.y = b.CreateSelect(accepted, quotient, state.y);
  } else {
    auto *error =
        operation == MathOp::divide_unsigned || mode.fractional ? b.getInt32(word_mask) : state.x;
    state.x = b.CreateSelect(accepted, quotient, error);
  }
  state.ccr = ccr;
}

void multiply(Builder &b, Values &state, const MathOp operation) {
  if (operation == MathOp::multiply_byte) {
    auto *a = b.CreateLShr(state.d, b.getInt32(byte_bits));
    auto *value = b.CreateAnd(state.d, b.getInt32(byte_mask));
    state.d = LLVMBuildMul(b.get(), a, value, "");
    auto *c = b.CreateAnd(b.CreateLShr(state.d, b.getInt32(byte_bits - 1)), b.getInt32(carry));
    state.ccr = b.CreateOr(b.CreateAnd(state.ccr, b.getInt32(byte_mask ^ carry)), c);
    return;
  }
  auto *left = state.d;
  auto *right = state.y;
  if (operation == MathOp::multiply_signed) {
    left = LLVMBuildSExt(b.get(), b.CreateTrunc(left, b.getInt16Ty()), b.getInt32Ty(), "");
    right = LLVMBuildSExt(b.get(), b.CreateTrunc(right, b.getInt16Ty()), b.getInt32Ty(), "");
  }
  auto *product = LLVMBuildMul(b.get(), left, right, "");
  state.d = b.CreateAnd(product, b.getInt32(word_mask));
  state.y = b.CreateLShr(product, b.getInt32(word_bits));
  state.ccr = flags(b,
                    state.ccr,
                    {.value = product,
                     .carry_bit = b.CreateAnd(b.CreateLShr(product, b.getInt32(word_bits - 1)),
                                              b.getInt32(carry)),
                     .overflow_bit = b.CreateAnd(state.ccr, b.getInt32(overflow)),
                     .width = 2 * word_bits});
}
}

void lower_math(Builder &builder, Values &state, const MathOp operation) {
  switch (operation) {
    case MathOp::multiply_byte:
    case MathOp::multiply_unsigned:
    case MathOp::multiply_signed: multiply(builder, state, operation); return;
    case MathOp::divide_unsigned:
    case MathOp::divide_signed:
    case MathOp::divide_extended:
    case MathOp::divide_extended_signed:
    case MathOp::divide_fractional: divide(builder, state, operation); return;
    default: throw std::logic_error("Unknown CPU12 LLVM multiply/divide operation");
  }
}
}
