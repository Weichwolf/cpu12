// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"

#include <array>
#include <stdexcept>

namespace cpu12::detail::a64 {
void Emitter::branch(const Instruction &operation) {
  const auto condition = operation.condition;
  if (condition == 0) {
    advance(operation.length + operation.operand);
    return;
  }
  advance(operation.length);
  if (condition == 1) { return; }
  Label not_taken;
  branch_guard(operation, not_taken);
  advance(operation.operand);
  code_.bind(not_taken);
}

void Emitter::branch_guard(const Instruction &operation, Label &not_taken) {
  const auto condition = operation.condition;
  if (condition >= 12) {
    code_.shift(Shift::right, Reg::x5, ccr, 2);
    code_.alu(Alu::bit_xor, Reg::x5, Reg::x5, ccr);
    mask(Reg::x5, Reg::x5, flag_v);
    if (condition >= 14) {
      mask(Reg::x6, ccr, flag_z);
      code_.alu(Alu::bit_or, Reg::x5, Reg::x5, Reg::x6);
    }
  } else {
    constexpr std::array masks{0U,
                               0U,
                               flag_c | flag_z,
                               flag_c | flag_z,
                               flag_c,
                               flag_c,
                               flag_z,
                               flag_z,
                               flag_v,
                               flag_v,
                               flag_n,
                               flag_n};
    mask(Reg::x5, ccr, masks.at(condition));
  }
  code_.branch_zero_far(Reg::x5, not_taken, Width::w32, (condition & 1U) == 0);
}

void Emitter::loop(const Instruction &operation) {
  load_target(Reg::x2, operation);
  if (operation.operation != RegisterOp::test) {
    code_.constant(Reg::x17, operation.operation == RegisterOp::increment ? 1U : word_mask);
    code_.alu(Alu::add, Reg::x2, Reg::x2, Reg::x17);
    code_.extract(Reg::x2, Reg::x2, 0, operation.word ? word_bits : byte_bits);
    store_target(Reg::x2, operation);
  }
  advance(operation.length);
  Label not_taken;
  code_.branch_zero_far(Reg::x2, not_taken, Width::w32, operation.condition == 0);
  advance(operation.operand);
  code_.bind(not_taken);
}

void Emitter::transfer_read(const Reg value, const unsigned selector, const bool sign) {
  switch (selector) {
    case 0: code_.extract(value, d, byte_bits, byte_bits, Width::w32, sign); break;
    case 1: code_.extract(value, d, 0, byte_bits, Width::w32, sign); break;
    case 2: code_.extract(value, ccr, 0, byte_bits, Width::w32, sign); break;
    case 4: code_.move(value, d, Width::w32); break;
    case 5: code_.move(value, x, Width::w32); break;
    case 6: code_.move(value, y, Width::w32); break;
    case 7: code_.move(value, stack, Width::w32); break;
    default: throw std::logic_error("Invalid CPU12 transfer register");
  }
}

void Emitter::transfer_write(const Reg value, const unsigned selector) {
  if (selector == 2) {
    code_.constant(Reg::x17, byte_mask ^ flag_x);
    code_.alu(Alu::bit_or, Reg::x6, ccr, Reg::x17);
    code_.alu(Alu::bit_and, ccr, value, Reg::x6);
    code_.extract(ccr, ccr, 0, byte_bits);
  } else if (selector < 2) {
    store_target(value, {.accumulator_b = selector != 0});
  } else {
    constexpr std::array targets{d, x, y, stack};
    code_.extract(targets.at(selector - 4), value, 0, word_bits);
  }
}

void Emitter::transfer(const Instruction &operation) {
  constexpr unsigned exchange_mask = 0x80;
  const auto source = (operation.operand >> 4U) & 7U;
  const auto destination = operation.operand & 7U;
  const bool exchange = (operation.operand & exchange_mask) != 0;
  transfer_read(Reg::x0, source, !exchange && destination >= 4);
  if (!exchange && source == 0 && destination == 2) { defer_interrupt(Reg::x0); }
  if (exchange) {
    transfer_read(Reg::x1, destination);
    if (source >= 4 && destination < 4 && destination != 0) {
      code_.constant(Reg::x17, word_mask ^ byte_mask);
      code_.alu(Alu::bit_or, Reg::x1, Reg::x1, Reg::x17);
    }
    transfer_write(Reg::x1, source);
  }
  transfer_write(Reg::x0, destination);
  constexpr unsigned prefix = 0x18;
  if (operation.opcode == prefix) {
    code_.move(Reg::x2, Reg::x0, Width::w32);
    flags(byte_bits);
    publish_flags();
  }
}
}
