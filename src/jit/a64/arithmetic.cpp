// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"

#include <stdexcept>

namespace cpu12::detail::a64 {
void Emitter::flags(const unsigned bits, const Flags policy) {
  mask(Reg::x4, ccr, byte_mask ^ policy.changed);
  if ((policy.changed & flag_n) != 0) {
    code_.extract(Reg::x5, Reg::x2, bits - 1, 1);
    code_.shift(Shift::left, Reg::x5, Reg::x5, 3);
    code_.alu(Alu::bit_or, Reg::x4, Reg::x4, Reg::x5);
  }
  if ((policy.changed & flag_z) != 0) {
    code_.extract(Reg::x5, Reg::x2, 0, bits);
    code_.compare(Reg::x5, Reg::zr);
    code_.set(Reg::x5, Condition::equal);
    code_.shift(Shift::left, Reg::x5, Reg::x5, 2);
    code_.alu(Alu::bit_or, Reg::x4, Reg::x4, Reg::x5);
  }
  if (policy.arithmetic && (policy.changed & flag_v) != 0) {
    code_.alu(Alu::bit_xor, Reg::x5, Reg::x0, Reg::x1);
    if (!policy.subtract) {
      code_.constant(Reg::x17, word_mask);
      code_.alu(Alu::bit_xor, Reg::x5, Reg::x5, Reg::x17);
    }
    code_.alu(Alu::bit_xor, Reg::x6, Reg::x0, Reg::x2);
    code_.alu(Alu::bit_and, Reg::x5, Reg::x5, Reg::x6);
    code_.extract(Reg::x5, Reg::x5, bits - 1, 1);
    code_.shift(Shift::left, Reg::x5, Reg::x5, 1);
    code_.alu(Alu::bit_or, Reg::x4, Reg::x4, Reg::x5);
  }
  if ((policy.changed & flag_c) != 0) {
    code_.extract(Reg::x5, Reg::x2, bits, 1);
    code_.alu(Alu::bit_or, Reg::x4, Reg::x4, Reg::x5);
  }
  if ((policy.changed & flag_h) != 0) {
    code_.alu(Alu::bit_xor, Reg::x5, Reg::x0, Reg::x1);
    code_.alu(Alu::bit_xor, Reg::x5, Reg::x5, Reg::x2);
    code_.extract(Reg::x5, Reg::x5, 4, 1);
    code_.shift(Shift::left, Reg::x5, Reg::x5, 5);
    code_.alu(Alu::bit_or, Reg::x4, Reg::x4, Reg::x5);
  }
}

void Emitter::arithmetic(const Instruction &operation) {
  const auto kind = operation.kind;
  if (kind == Kind::store) {
    address(operation);
    load_target(Reg::x0, operation);
    if (operation.word && operation.addressing == Addressing::indexed &&
        word_target(operation.target) == index_base(operation.indexed.base)) {
      add_word(Reg::x0, Reg::x0, operation.indexed.update);
    }
    data_access({.word = operation.word, .write = true});
    commit_index(operation);
    load_target(Reg::x2, operation);
  } else {
    operand(operation);
    load_target(Reg::x0, operation);
    switch (kind) {
      case Kind::load: code_.move(Reg::x2, Reg::x1, Width::w32); break;
      case Kind::add:
      case Kind::add_carry: code_.alu(Alu::add, Reg::x2, Reg::x0, Reg::x1); break;
      case Kind::subtract:
      case Kind::subtract_carry:
      case Kind::compare: code_.alu(Alu::subtract, Reg::x2, Reg::x0, Reg::x1); break;
      case Kind::bit_and:
      case Kind::bit_test: code_.alu(Alu::bit_and, Reg::x2, Reg::x0, Reg::x1); break;
      case Kind::bit_or: code_.alu(Alu::bit_or, Reg::x2, Reg::x0, Reg::x1); break;
      case Kind::bit_xor: code_.alu(Alu::bit_xor, Reg::x2, Reg::x0, Reg::x1); break;
      case Kind::nop:
      case Kind::branch:
      case Kind::store:
      case Kind::effective_address:
      case Kind::jump:
      case Kind::call:
      case Kind::push:
      case Kind::pull:
      case Kind::return_subroutine:
      case Kind::register_operation:
      case Kind::transfer:
      case Kind::memory_unary:
      case Kind::loop:
      case Kind::memory_bits:
      case Kind::bit_branch:
      case Kind::move:
      case Kind::multiply_divide:
      case Kind::return_interrupt:
      case Kind::software_interrupt:
      case Kind::wait:
      case Kind::stop:
      default: throw std::logic_error("Not an A64 arithmetic instruction");
    }
    if (kind == Kind::add_carry || kind == Kind::subtract_carry) {
      code_.extract(Reg::x3, ccr, 0, 1);
      code_.alu(kind == Kind::add_carry ? Alu::add : Alu::subtract, Reg::x2, Reg::x2, Reg::x3);
    }
  }
  const bool add = kind == Kind::add || kind == Kind::add_carry;
  const bool subtract =
      kind == Kind::subtract || kind == Kind::subtract_carry || kind == Kind::compare;
  flags(operation.word ? word_bits : byte_bits,
        {.changed = flag_n | flag_z | flag_v | (add || subtract ? flag_c : 0U) |
                    (add && !operation.word ? flag_h : 0U),
         .subtract = subtract,
         .arithmetic = add || subtract});
  code_.move(ccr, Reg::x4, Width::w32);
  if (kind != Kind::store && kind != Kind::compare && kind != Kind::bit_test) {
    store_target(Reg::x2, operation);
  }
}
}
