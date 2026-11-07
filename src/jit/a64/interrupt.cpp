// SPDX-License-Identifier: GPL-3.0-only
#include "jit/interrupt.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"

#include <array>
#include <vector>

namespace cpu12::detail::a64 {
void Emitter::stack_context(const Byte next_pc) {
  struct Part {
    Reg source;
    int position;
    bool word = true;
  };

  constexpr std::array parts{Part{.source = pc, .position = 2},
                             Part{.source = y, .position = 4},
                             Part{.source = x, .position = 6},
                             Part{.source = d, .position = 8},
                             Part{.source = ccr, .position = 9, .word = false}};
  for (const auto part : parts) {
    add_word(Reg::x9, stack, -part.position);
    if (part.source == pc) {
      add_word(Reg::x0, pc, next_pc);
    } else if (part.source == d) {
      code_.reverse_half(Reg::x0, d);
    } else {
      code_.move(Reg::x0, part.source, Width::w32);
    }
    data_access({.word = part.word, .write = true});
  }
}

std::vector<Byte>
Emitter::interrupt(const InterruptRequest request, const InterruptFrame frame, const Byte next_pc) {
  begin(false);
  code_.constant(Reg::x9, request.vector);
  data_access({.word = true});
  code_.store(Reg::x1, Reg::sp, 16);
  if (frame == InterruptFrame::push) { stack_context(next_pc); }
  if (frame != InterruptFrame::waiting) { add_word(stack, stack, -9); }
  code_.constant(Reg::x17, flag_i | (request.kind == InterruptKind::xirq ? flag_x : 0U));
  code_.alu(Alu::bit_or, ccr, ccr, Reg::x17);
  code_.load(pc, Reg::sp, 16);
  state_byte(StateField::execution, static_cast<Byte>(ExecutionState::running));
  state_byte(StateField::irq_delay, 0);
  state_byte(StateField::returned_frame, 0);
  return finish();
}

void Emitter::return_interrupt() {
  struct Part {
    Reg target;
    unsigned position, slot;
    bool word = true;
  };

  constexpr std::array parts{Part{.target = ccr, .position = 0, .slot = 0, .word = false},
                             Part{.target = d, .position = 1, .slot = 2},
                             Part{.target = x, .position = 3, .slot = 4},
                             Part{.target = pc, .position = 7, .slot = 6},
                             Part{.target = y, .position = 5, .slot = 8}};
  for (const auto part : parts) {
    add_word(Reg::x9, stack, static_cast<int>(part.position));
    data_access({.word = part.word});
    code_.store(Reg::x1, Reg::sp, part.slot, part.word ? Access::half : Access::byte);
  }
  for (const auto part : parts) {
    code_.load(Reg::x1, Reg::sp, part.slot, part.word ? Access::half : Access::byte);
    if (part.target == d) {
      code_.reverse_half(d, Reg::x1);
    } else if (part.target == ccr) {
      transfer_write(Reg::x1, 2);
    } else {
      code_.move(part.target, Reg::x1, Width::w32);
    }
  }
  add_word(stack, stack, 9);
  state_byte(StateField::irq_delay, 0);
  state_byte(StateField::returned_frame, 1);
}

void Emitter::wait(const Instruction &operation) {
  Label complete;
  if (operation.kind == Kind::stop) {
    mask(Reg::x5, ccr, flag_s);
    code_.branch_zero_far(Reg::x5, complete, Width::w32, true);
  }
  stack_context(operation.length);
  add_word(stack, stack, -9);
  state_byte(StateField::execution,
             static_cast<Byte>(operation.kind == Kind::stop ? ExecutionState::stopped
                                                            : ExecutionState::waiting));
  code_.bind(complete);
  advance(operation.length);
}

std::vector<Byte> Emitter::wake() {
  begin(false);
  add_word(stack, stack, 9);
  state_byte(StateField::execution, static_cast<Byte>(ExecutionState::running));
  return finish();
}
}
