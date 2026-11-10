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
constexpr std::uint32_t half = 0x20;

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

LLVMValueRef &word(Values &values, const WordTarget target) {
  switch (target) {
    case WordTarget::d: return values.d;
    case WordTarget::x: return values.x;
    case WordTarget::y: return values.y;
    case WordTarget::sp: return values.sp;
    default: throw std::logic_error("Invalid CPU12 LLVM word register");
  }
}

void write(Builder &b, Values &state, const Instruction instruction, LLVMValueRef value) {
  if (instruction.word) {
    word(state, instruction.target) = value;
  } else if (instruction.accumulator_b) {
    state.d = b.CreateOr(b.CreateAnd(state.d, b.getInt32(word_mask ^ byte_mask)), value);
  } else {
    state.d = b.CreateOr(b.CreateAnd(state.d, b.getInt32(byte_mask)),
                         b.CreateShl(value, b.getInt32(byte_bits)));
  }
}

void arithmetic(Builder &b, Values &s, const Instruction instruction, LLVMValueRef right) {
  const bool add = instruction.kind == Kind::add || instruction.kind == Kind::add_carry;
  const bool with_carry =
      instruction.kind == Kind::add_carry || instruction.kind == Kind::subtract_carry;
  const auto width = instruction.word ? word_bits : byte_bits;
  const auto mask = instruction.word ? word_mask : byte_mask;
  auto *left = register_value(b, s, instruction);
  auto *adjusted = with_carry ? b.CreateAdd(right, b.CreateAnd(s.ccr, b.getInt32(carry))) : right;
  auto *wide = add ? b.CreateAdd(left, adjusted) : b.CreateSub(left, adjusted);
  auto *result = b.CreateAnd(wide, b.getInt32(mask));
  auto *c = flag(
      b, add ? b.CreateICmpUGT(wide, b.getInt32(mask)) : b.CreateICmpULT(left, adjusted), carry);
  auto *different = b.CreateXor(left, right);
  auto *signs = b.CreateAnd(add ? b.CreateNot(different) : different, b.CreateXor(left, result));
  auto *v = flag(b,
                 b.CreateICmpNE(b.CreateAnd(signs, b.getInt32(1U << (width - 1))), b.getInt32(0)),
                 overflow);
  s.ccr = flags(b, s.ccr, {.value = result, .carry_bit = c, .overflow_bit = v, .width = width});
  if (add && !instruction.word) {
    auto *h = b.CreateAnd(b.CreateXor(different, result), b.getInt32(16));
    s.ccr =
        b.CreateOr(b.CreateAnd(s.ccr, b.getInt32(byte_mask ^ half)), b.CreateShl(h, b.getInt32(1)));
  }
  if (instruction.kind != Kind::compare) { write(b, s, instruction, result); }
}

LLVMValueRef logical(Builder &b, Values &s, const Instruction instruction, LLVMValueRef right) {
  auto *left = register_value(b, s, instruction);
  if (instruction.kind == Kind::load || instruction.kind == Kind::store) { return right; }
  if (instruction.kind == Kind::bit_and || instruction.kind == Kind::bit_test) {
    return b.CreateAnd(left, right);
  }
  if (instruction.kind == Kind::bit_or) { return b.CreateOr(left, right); }
  if (instruction.kind == Kind::bit_xor) { return b.CreateXor(left, right); }
  throw std::logic_error("Invalid CPU12 LLVM logical instruction");
}

}

LLVMValueRef register_value(Builder &b, Values &state, const Instruction instruction) {
  if (instruction.word) { return word(state, instruction.target); }
  return instruction.accumulator_b ? b.CreateAnd(state.d, b.getInt32(byte_mask))
                                   : b.CreateLShr(state.d, b.getInt32(byte_bits));
}

void lower(Builder &b, Values &s, const Instruction instruction, LLVMValueRef operand) {
  if (instruction.kind == Kind::register_operation &&
      (instruction.operation == RegisterOp::index_increment ||
       instruction.operation == RegisterOp::index_decrement)) {
    auto &value = instruction.target == WordTarget::x ? s.x : s.y;
    const auto delta = instruction.operation == RegisterOp::index_increment ? 1U : word_mask;
    value = b.CreateAnd(b.CreateAdd(value, b.getInt32(delta)), b.getInt32(word_mask));
    s.ccr = b.CreateOr(b.CreateAnd(s.ccr, b.getInt32(byte_mask ^ zero)),
                       flag(b, b.CreateICmpEQ(value, b.getInt32(0)), zero));
    return;
  }
  if (operand == nullptr) {
    if (instruction.addressing != Addressing::immediate) {
      throw std::runtime_error("LLVM lowering requires an established operand");
    }
    operand = b.getInt32(instruction.operand);
  }
  if (instruction.kind == Kind::add || instruction.kind == Kind::add_carry ||
      instruction.kind == Kind::subtract || instruction.kind == Kind::subtract_carry ||
      instruction.kind == Kind::compare) {
    arithmetic(b, s, instruction, operand);
    return;
  }
  auto *result = logical(b, s, instruction, operand);
  s.ccr = flags(b,
                s.ccr,
                {.value = result,
                 .carry_bit = b.CreateAnd(s.ccr, b.getInt32(carry)),
                 .overflow_bit = b.getInt32(0),
                 .width = instruction.word ? word_bits : byte_bits});
  if (instruction.kind != Kind::bit_test && instruction.kind != Kind::store) {
    write(b, s, instruction, result);
  }
}

}
