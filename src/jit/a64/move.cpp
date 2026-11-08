// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"

namespace cpu12::detail::a64 {
void Emitter::memory_modify(const Instruction &operation) {
  address(operation);
  if (operation.kind == Kind::bit_branch) {
    data_access({.selected = true, .mask = operation.bit_mask});
    code_.constant(Reg::x17, operation.condition == 0 ? operation.bit_mask : 0U);
    code_.compare(Reg::x1, Reg::x17);
    code_.set(Reg::x2, Condition::equal);
    commit_index(operation);
    advance(operation.length);
    Label not_taken;
    code_.branch_zero_far(Reg::x2, not_taken);
    advance(operation.operand);
    code_.bind(not_taken);
    return;
  }
  code_.store(Reg::x9, Reg::sp, 0);
  if (operation.kind == Kind::memory_unary && operation.operation == RegisterOp::clear) {
    code_.constant(Reg::x0, 0);
  } else {
    data_access({});
    code_.move(Reg::x0, Reg::x1, Width::w32);
  }
  if (operation.kind == Kind::memory_bits) {
    code_.constant(Reg::x17,
                   operation.condition == 0 ? operation.bit_mask : byte_mask ^ operation.bit_mask);
    code_.alu(operation.condition == 0 ? Alu::bit_or : Alu::bit_and, Reg::x2, Reg::x0, Reg::x17);
    flags(byte_bits);
  } else {
    unary_value(operation);
  }
  if (operation.kind == Kind::memory_unary && operation.operation == RegisterOp::test) {
    publish_flags();
  } else {
    code_.store(Reg::x2, Reg::sp, 8);
    code_.store(Reg::x4, Reg::sp, 16);
    code_.load(Reg::x9, Reg::sp, 0);
    code_.load(Reg::x0, Reg::sp, 8);
    data_access({.write = true});
    code_.load(ccr, Reg::sp, 16);
  }
  commit_index(operation);
  advance(operation.length);
}

void Emitter::move(const Instruction &operation) {
  const auto descriptor = [&](const MoveOperand operand) {
    return Instruction{.kind = Kind::move,
                       .length = operation.length,
                       .addressing = operand.addressing,
                       .address = operand.value,
                       .word = operation.word,
                       .indexed = operand.indexed};
  };
  const auto source = descriptor(operation.move_source);
  const auto destination = descriptor(operation.move_destination);
  if (source.addressing == Addressing::immediate) {
    code_.constant(Reg::x1, source.address);
  } else {
    address(source);
    data_access({.word = operation.word});
  }
  code_.store(Reg::x1, Reg::sp, 8);
  address(destination);
  if (source.addressing == Addressing::indexed && destination.addressing == Addressing::indexed &&
      source.indexed.base == destination.indexed.base) {
    add_word(Reg::x9, Reg::x9, source.indexed.update);
  }
  code_.load(Reg::x0, Reg::sp, 8);
  data_access({.word = operation.word, .write = true});
  commit_index(source);
  commit_index(destination);
}
}
