// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"
#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace cpu12::detail::a64 {
void Emitter::unary_value(const Instruction &operation) {
  const auto unary = operation.operation;
  const auto bits = operation.word ? word_bits : byte_bits;
  code_.constant(Reg::x1, 1);
  switch (unary) {
    case RegisterOp::negate:
      code_.move(Reg::x1, Reg::x0, Width::w32);
      code_.constant(Reg::x0, 0);
      code_.alu(Alu::subtract, Reg::x2, Reg::x0, Reg::x1);
      break;
    case RegisterOp::complement:
      code_.constant(Reg::x17, operation.word ? word_mask : byte_mask);
      code_.alu(Alu::bit_xor, Reg::x2, Reg::x0, Reg::x17);
      break;
    case RegisterOp::increment: code_.alu(Alu::add, Reg::x2, Reg::x0, Reg::x1); break;
    case RegisterOp::decrement: code_.alu(Alu::subtract, Reg::x2, Reg::x0, Reg::x1); break;
    case RegisterOp::clear: code_.constant(Reg::x2, 0); break;
    case RegisterOp::test: code_.move(Reg::x2, Reg::x0, Width::w32); break;
    case RegisterOp::logical_right:
    case RegisterOp::rotate_right:
    case RegisterOp::arithmetic_right:
      code_.extract(Reg::x2, Reg::x0, 0, bits, Width::w32, unary == RegisterOp::arithmetic_right);
      code_.shift(unary == RegisterOp::arithmetic_right ? Shift::arithmetic : Shift::right,
                  Reg::x2,
                  Reg::x2,
                  1);
      code_.extract(Reg::x2, Reg::x2, 0, bits);
      if (unary == RegisterOp::rotate_right) {
        code_.extract(Reg::x3, ccr, 0, 1);
        code_.shift(Shift::left, Reg::x3, Reg::x3, bits - 1);
        code_.alu(Alu::bit_or, Reg::x2, Reg::x2, Reg::x3);
      }
      code_.extract(Reg::x3, Reg::x0, 0, 1);
      code_.shift(Shift::left, Reg::x3, Reg::x3, bits);
      code_.alu(Alu::bit_or, Reg::x2, Reg::x2, Reg::x3);
      break;
    case RegisterOp::rotate_left:
    case RegisterOp::arithmetic_left:
      code_.shift(Shift::left, Reg::x2, Reg::x0, 1);
      if (unary == RegisterOp::rotate_left) {
        code_.extract(Reg::x3, ccr, 0, 1);
        code_.alu(Alu::bit_or, Reg::x2, Reg::x2, Reg::x3);
      }
      break;
    case RegisterOp::index_increment:
    case RegisterOp::index_decrement:
    case RegisterOp::ccr_and:
    case RegisterOp::ccr_or:
    case RegisterOp::add_b:
    case RegisterOp::subtract_b:
    case RegisterOp::compare_b:
    case RegisterOp::decimal_adjust:
    default: throw std::logic_error("Not a CPU12 unary operation");
  }
  if (!policy_.flags_live) { return; }
  const bool inc_dec = unary == RegisterOp::increment || unary == RegisterOp::decrement;
  flags(bits,
        {.changed = flag_n | flag_z | flag_v | (inc_dec ? 0U : flag_c),
         .subtract = unary == RegisterOp::decrement || unary == RegisterOp::negate,
         .arithmetic = inc_dec || unary == RegisterOp::negate});
  if (unary >= RegisterOp::logical_right && unary <= RegisterOp::arithmetic_left) {
    code_.extract(Reg::x5, Reg::x4, 3, 1);
    code_.extract(Reg::x6, Reg::x4, 0, 1);
    code_.alu(Alu::bit_xor, Reg::x5, Reg::x5, Reg::x6);
    code_.shift(Shift::left, Reg::x5, Reg::x5, 1);
    code_.alu(Alu::bit_or, Reg::x4, Reg::x4, Reg::x5);
  } else if (unary == RegisterOp::complement) {
    code_.constant(Reg::x17, flag_c);
    code_.alu(Alu::bit_or, Reg::x4, Reg::x4, Reg::x17);
  }
}

void Emitter::defer_interrupt(const Reg candidate) {
  Label unchanged;
  mask(Reg::x5, ccr, flag_i);
  code_.branch_zero_far(Reg::x5, unchanged);
  mask(Reg::x5, candidate, flag_i);
  code_.branch_zero_far(Reg::x5, unchanged, Width::w32, true);
  state_byte(StateField::irq_delay, 2);
  code_.bind(unchanged);
}

void Emitter::special_register(const Instruction &operation) {
  const auto unary = operation.operation;
  if (unary == RegisterOp::index_increment || unary == RegisterOp::index_decrement) {
    const auto target = word_target(operation.target);
    add_word(target, target, unary == RegisterOp::index_increment ? 1 : -1);
    code_.move(Reg::x2, target, Width::w32);
    flags(word_bits, {.changed = flag_z});
    publish_flags();
  } else if (unary == RegisterOp::ccr_and || unary == RegisterOp::ccr_or) {
    code_.constant(Reg::x17,
                   unary == RegisterOp::ccr_or ? operation.operand & (byte_mask ^ flag_x)
                                               : operation.operand);
    code_.alu(unary == RegisterOp::ccr_and ? Alu::bit_and : Alu::bit_or, Reg::x2, ccr, Reg::x17);
    defer_interrupt(Reg::x2);
    code_.move(ccr, Reg::x2, Width::w32);
  } else if (unary == RegisterOp::decimal_adjust) {
    decimal_adjust();
  } else {
    const bool add = unary == RegisterOp::add_b;
    code_.extract(Reg::x0, d, byte_bits, byte_bits);
    code_.extract(Reg::x1, d, 0, byte_bits);
    code_.alu(add ? Alu::add : Alu::subtract, Reg::x2, Reg::x0, Reg::x1);
    flags(byte_bits,
          {.changed = flag_n | flag_z | flag_v | flag_c | (add ? flag_h : 0U),
           .subtract = !add,
           .arithmetic = true});
    publish_flags();
    if (unary != RegisterOp::compare_b) { store_target(Reg::x2, {}); }
  }
}

void Emitter::unary(const Instruction &operation) {
  if (operation.operation > RegisterOp::test) {
    special_register(operation);
    return;
  }
  load_target(Reg::x0, operation);
  unary_value(operation);
  if (operation.operation != RegisterOp::test) { store_target(Reg::x2, operation); }
  publish_flags();
}

void Emitter::decimal_adjust() {
  Label invalid;
  Label low_valid;
  Label high_valid;
  Label no_low;
  Label no_high;
  Label complete;
  code_.extract(Reg::x0, d, byte_bits, byte_bits);
  mask(Reg::x1, ccr, flag_h);
  mask(Reg::x2, Reg::x0, 15);
  code_.branch_zero_far(Reg::x1, low_valid);
  code_.constant(Reg::x17, 3);
  code_.compare(Reg::x2, Reg::x17);
  code_.branch_far(Condition::above, invalid);
  code_.bind(low_valid);
  mask(Reg::x3, ccr, flag_c);
  code_.branch_zero_far(Reg::x3, high_valid);
  code_.constant(Reg::x5, 3);
  code_.constant(Reg::x6, 2);
  code_.compare(Reg::x1, Reg::zr);
  code_.select(Reg::x5, Reg::x5, Reg::x6, Condition::not_equal);
  code_.shift(Shift::right, Reg::x6, Reg::x0, 4);
  code_.compare(Reg::x6, Reg::x5);
  code_.branch_far(Condition::above, invalid);
  code_.bind(high_valid);
  constexpr unsigned upper_limit = 0x99;
  code_.constant(Reg::x17, upper_limit);
  code_.compare(Reg::x0, Reg::x17);
  code_.set(Reg::x5, Condition::above);
  code_.alu(Alu::bit_or, Reg::x3, Reg::x3, Reg::x5);
  code_.constant(Reg::x17, 9);
  code_.compare(Reg::x2, Reg::x17);
  code_.set(Reg::x5, Condition::above);
  code_.alu(Alu::bit_or, Reg::x5, Reg::x5, Reg::x1);
  code_.move(Reg::x2, Reg::x0, Width::w32);
  code_.branch_zero_far(Reg::x5, no_low);
  code_.add(Reg::x2, Reg::x2, 6);
  code_.bind(no_low);
  code_.branch_zero_far(Reg::x3, no_high);
  constexpr unsigned correction = 0x60;
  code_.add(Reg::x2, Reg::x2, correction);
  code_.bind(no_high);
  code_.extract(Reg::x2, Reg::x2, 0, byte_bits);
  flags(byte_bits, {.changed = flag_n | flag_z | flag_c});
  code_.alu(Alu::bit_or, ccr, Reg::x4, Reg::x3);
  store_target(Reg::x2, {});
  code_.branch(complete);
  code_.bind(invalid);
  code_.extract(Reg::x5, ccr, 0, 1);
  code_.shift(Shift::left, Reg::x5, Reg::x5, byte_bits);
  code_.alu(Alu::bit_or, Reg::x0, Reg::x0, Reg::x5);
  code_.extract(Reg::x5, ccr, 5, 1);
  code_.shift(Shift::left, Reg::x5, Reg::x5, byte_bits + 1);
  code_.alu(Alu::bit_or, Reg::x0, Reg::x0, Reg::x5);
  code_.store(Reg::x0, context, offsetof(NativeContext, value), Access::half);
  code_.store(Reg::zr, context, offsetof(NativeContext, address), Access::half);
  code_.store(Reg::zr, context, offsetof(NativeContext, access_size), Access::byte);
  code_.constant(Reg::x17, static_cast<std::uint8_t>(Fault::unsupported_state));
  code_.store(Reg::x17, context, offsetof(NativeContext, fault), Access::byte);
  code_.branch(failure_);
  code_.bind(complete);
}
}
