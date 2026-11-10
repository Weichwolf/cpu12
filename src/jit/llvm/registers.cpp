// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/registers.hpp"

#include "instruction.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/flags.hpp"
#include "jit/llvm/lift.hpp"

#include "llvm-c/Types.h"

#include <stdexcept>

namespace cpu12::detail::llvm19 {
namespace {
constexpr unsigned byte_bits = 8, word_bits = 16, byte_mask = 0xff, word_mask = 0xffff;
constexpr unsigned carry = 1, overflow = 2, half = 0x20, xirq = 0x40, zero = 4;

LLVMValueRef bit(Builder &b, LLVMValueRef condition, const unsigned mask) {
  return b.CreateSelect(condition, b.getInt32(mask), b.getInt32(0));
}

struct Unary {
  LLVMValueRef result, c, v;
};

Unary unary(Builder &b, const Values &s, const Instruction instruction, LLVMValueRef source) {
  const auto width = instruction.word ? word_bits : byte_bits;
  const auto sign = 1U << (width - 1);
  const auto mask = instruction.word ? word_mask : byte_mask;
  const auto op = instruction.operation;
  auto *result = source;
  auto *c = b.getInt32(0);
  auto *v = b.getInt32(0);
  switch (op) {
    case RegisterOp::negate:
      result = b.CreateSub(b.getInt32(0), source);
      c = bit(b, b.CreateICmpNE(source, b.getInt32(0)), carry);
      v = bit(b, b.CreateICmpEQ(source, b.getInt32(sign)), overflow);
      break;
    case RegisterOp::complement:
      result = b.CreateXor(source, b.getInt32(mask));
      c = b.getInt32(carry);
      break;
    case RegisterOp::increment:
      result = b.CreateAdd(source, b.getInt32(1));
      c = b.CreateAnd(s.ccr, b.getInt32(carry));
      v = bit(b, b.CreateICmpEQ(source, b.getInt32(sign - 1)), overflow);
      break;
    case RegisterOp::decrement:
      result = b.CreateSub(source, b.getInt32(1));
      c = b.CreateAnd(s.ccr, b.getInt32(carry));
      v = bit(b, b.CreateICmpEQ(source, b.getInt32(sign)), overflow);
      break;
    case RegisterOp::logical_right:
    case RegisterOp::rotate_right:
    case RegisterOp::arithmetic_right:
      result = b.CreateLShr(source, b.getInt32(1));
      c = b.CreateAnd(source, b.getInt32(carry));
      if (op == RegisterOp::rotate_right) {
        result = b.CreateOr(
            result, b.CreateShl(b.CreateAnd(s.ccr, b.getInt32(carry)), b.getInt32(width - 1)));
      } else if (op == RegisterOp::arithmetic_right) {
        result = b.CreateOr(result, b.CreateAnd(source, b.getInt32(sign)));
      }
      break;
    case RegisterOp::rotate_left:
    case RegisterOp::arithmetic_left:
      result = b.CreateShl(source, b.getInt32(1));
      c = b.CreateLShr(source, b.getInt32(width - 1));
      if (op == RegisterOp::rotate_left) {
        result = b.CreateOr(result, b.CreateAnd(s.ccr, b.getInt32(carry)));
      }
      break;
    case RegisterOp::clear: result = b.getInt32(0); break;
    case RegisterOp::test: break;
    case RegisterOp::index_increment:
    case RegisterOp::index_decrement:
    case RegisterOp::ccr_and:
    case RegisterOp::ccr_or:
    case RegisterOp::add_b:
    case RegisterOp::subtract_b:
    case RegisterOp::compare_b:
    case RegisterOp::decimal_adjust:
    default: throw std::logic_error("Not a CPU12 LLVM unary operation");
  }
  result = b.CreateAnd(result, b.getInt32(mask));
  if (op >= RegisterOp::logical_right && op <= RegisterOp::arithmetic_left) {
    v = b.CreateShl(b.CreateXor(b.CreateLShr(result, b.getInt32(width - 1)), c), b.getInt32(1));
  }
  return {.result = result, .c = c, .v = v};
}

void decimal(Builder &b, Values &s, const Instruction instruction) {
  constexpr unsigned decimal_limit = 0x99;
  constexpr unsigned high_correction = 0x60;
  auto *a = b.CreateLShr(s.d, b.getInt32(byte_bits));
  auto *c = b.CreateOr(b.CreateICmpNE(b.CreateAnd(s.ccr, b.getInt32(carry)), b.getInt32(0)),
                       b.CreateICmpUGT(a, b.getInt32(decimal_limit)));
  auto *h = b.CreateOr(b.CreateICmpNE(b.CreateAnd(s.ccr, b.getInt32(half)), b.getInt32(0)),
                       b.CreateICmpUGT(b.CreateAnd(a, b.getInt32(15)), b.getInt32(9)));
  auto *correction = b.CreateAdd(bit(b, h, 6), bit(b, c, high_correction));
  auto *result = b.CreateAnd(b.CreateAdd(a, correction), b.getInt32(byte_mask));
  s.ccr = flags(b,
                s.ccr,
                {.value = result,
                 .carry_bit = bit(b, c, carry),
                 .overflow_bit = b.CreateAnd(s.ccr, b.getInt32(overflow)),
                 .width = byte_bits});
  register_value(b, s, instruction, result);
}
}

DecimalDomain decimal_domain(Builder &b, const Values &s) {
  auto *a = b.CreateLShr(s.d, b.getInt32(byte_bits));
  auto *c = b.CreateAnd(s.ccr, b.getInt32(carry));
  auto *h = b.CreateAnd(s.ccr, b.getInt32(half));
  auto *low = b.CreateOr(b.CreateICmpEQ(h, b.getInt32(0)),
                         b.CreateICmpULT(b.CreateAnd(a, b.getInt32(15)), b.getInt32(4)));
  auto *high_limit = b.CreateSelect(b.CreateICmpEQ(h, b.getInt32(0)), b.getInt32(3), b.getInt32(4));
  auto *high = b.CreateOr(b.CreateICmpEQ(c, b.getInt32(0)),
                          b.CreateICmpULT(b.CreateLShr(a, b.getInt32(4)), high_limit));
  auto *input = b.CreateOr(
      a, b.CreateOr(b.CreateShl(c, b.getInt32(byte_bits)), b.CreateShl(h, b.getInt32(4))));
  return {.input = input, .valid = b.CreateAnd(low, high)};
}

void lower_register(Builder &b, Values &s, const Instruction instruction) {
  const auto op = instruction.operation;
  if (op == RegisterOp::index_increment || op == RegisterOp::index_decrement) {
    auto &value = instruction.target == WordTarget::x ? s.x : s.y;
    const auto delta = op == RegisterOp::index_increment ? 1U : word_mask;
    value = b.CreateAnd(b.CreateAdd(value, b.getInt32(delta)), b.getInt32(word_mask));
    s.ccr = b.CreateOr(b.CreateAnd(s.ccr, b.getInt32(byte_mask ^ zero)),
                       bit(b, b.CreateICmpEQ(value, b.getInt32(0)), zero));
  } else if (op == RegisterOp::ccr_and || op == RegisterOp::ccr_or) {
    s.ccr = op == RegisterOp::ccr_and
                ? b.CreateAnd(s.ccr, b.getInt32(instruction.operand))
                : b.CreateOr(s.ccr, b.getInt32(instruction.operand & (byte_mask ^ xirq)));
  } else if (op == RegisterOp::decimal_adjust) {
    decimal(b, s, instruction);
  } else if (op == RegisterOp::add_b || op == RegisterOp::subtract_b ||
             op == RegisterOp::compare_b) {
    auto arithmetic = instruction;
    arithmetic.kind = Kind::compare;
    if (op == RegisterOp::add_b) {
      arithmetic.kind = Kind::add;
    } else if (op == RegisterOp::subtract_b) {
      arithmetic.kind = Kind::subtract;
    }
    lower(b, s, arithmetic, b.CreateAnd(s.d, b.getInt32(byte_mask)));
  } else {
    const auto result = unary(b, s, instruction, register_value(b, s, instruction));
    s.ccr = flags(b,
                  s.ccr,
                  {.value = result.result,
                   .carry_bit = result.c,
                   .overflow_bit = result.v,
                   .width = instruction.word ? word_bits : byte_bits});
    if (op != RegisterOp::test) { register_value(b, s, instruction, result.result); }
  }
}
}
