// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/lift.hpp"

#include "llvm-c/Types.h"

#include <cstdint>
#include <stdexcept>

namespace cpu12::detail::llvm19 {
namespace {
constexpr unsigned word_bits = 16, byte_bits = 8;
constexpr std::uint32_t word_mask = 0xffff, byte_mask = 0xff;
constexpr std::uint32_t carry = 1, overflow = 2, zero = 4, negative = 8;

LLVMValueRef flag(Builder &b, LLVMValueRef condition, const unsigned bit) {
  return b.CreateSelect(condition, b.getInt32(bit), b.getInt32(0));
}

struct FlagInputs {
  LLVMValueRef value, carry_bit, overflow_bit;
  unsigned width;
};

LLVMValueRef flags(Builder &b, LLVMValueRef old, const FlagInputs input) {
  const auto sign = std::uint32_t{1} << (input.width - 1);
  auto *bits = b.CreateOr(
      flag(b, b.CreateICmpEQ(input.value, b.getInt32(0)), zero),
      flag(b, b.CreateICmpNE(b.CreateAnd(input.value, b.getInt32(sign)), b.getInt32(0)), negative));
  bits = b.CreateOr(bits, input.carry_bit);
  bits = b.CreateOr(bits, input.overflow_bit);
  return b.CreateOr(b.CreateAnd(old, b.getInt32(byte_mask ^ (carry | overflow | zero | negative))),
                    bits);
}

}

void lower(Builder &b, Values &s, const Instruction instruction) {
  if (instruction.addressing != cpu12::detail::Addressing::immediate) {
    throw std::runtime_error("LLVM lowering requires an established immediate operand");
  }
  const auto operand = instruction.operand;
  if ((instruction.kind == Kind::add || instruction.kind == Kind::subtract) && instruction.word) {
    auto *left = s.d;
    auto *right = b.getInt32(operand);
    const bool subtract = instruction.kind == Kind::subtract;
    auto *wide = subtract ? b.CreateSub(left, right) : b.CreateAdd(left, right);
    auto *result = b.CreateAnd(wide, b.getInt32(word_mask));
    auto *c =
        flag(b,
             subtract ? b.CreateICmpULT(left, right) : b.CreateICmpUGT(wide, b.getInt32(word_mask)),
             carry);
    auto *different = b.CreateXor(left, right);
    auto *signs =
        b.CreateAnd(subtract ? different : b.CreateNot(different), b.CreateXor(left, result));
    auto *v =
        flag(b,
             b.CreateICmpNE(b.CreateAnd(signs, b.getInt32(1U << (word_bits - 1))), b.getInt32(0)),
             overflow);
    s.ccr =
        flags(b, s.ccr, {.value = result, .carry_bit = c, .overflow_bit = v, .width = word_bits});
    s.d = result;
    return;
  }
  if (instruction.kind == Kind::bit_xor && !instruction.word && !instruction.accumulator_b) {
    auto *a = b.CreateXor(b.CreateLShr(s.d, b.getInt32(byte_bits)), b.getInt32(operand));
    s.d =
        b.CreateOr(b.CreateShl(a, b.getInt32(byte_bits)), b.CreateAnd(s.d, b.getInt32(byte_mask)));
    s.ccr = flags(b,
                  s.ccr,
                  {.value = a,
                   .carry_bit = b.CreateAnd(s.ccr, b.getInt32(carry)),
                   .overflow_bit = b.getInt32(0),
                   .width = byte_bits});
    return;
  }
  throw std::runtime_error("Unsupported CPU12 LLVM arithmetic lowering");
}

}
