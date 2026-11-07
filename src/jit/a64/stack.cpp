// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"

#include <array>

namespace cpu12::detail::a64 {
void Emitter::stack_operation(const Instruction &operation) {
  const bool push = operation.kind == Kind::push;
  const int width = operation.word ? 2 : 1;
  add_word(Reg::x9, stack, push ? -width : 0);
  constexpr std::array selectors{5U, 6U, 0U, 1U, 2U, 4U, 0U};
  const auto selector = selectors.at(static_cast<unsigned>(operation.stack_target));
  if (push) { transfer_read(Reg::x0, selector); }
  data_access({.word = operation.word, .write = push});
  if (!push) {
    if (operation.stack_target == StackTarget::pc) {
      code_.move(pc, Reg::x1, Width::w32);
    } else {
      transfer_write(Reg::x1, selector);
    }
  }
  add_word(stack, stack, push ? -width : width);
  if (operation.kind != Kind::return_subroutine) { advance(operation.length); }
}

void Emitter::subroutine(const Instruction &operation) {
  if (operation.addressing == Addressing::relative) {
    add_word(Reg::x9, pc, operation.length + operation.operand);
  } else {
    address(operation);
  }
  code_.store(Reg::x9, Reg::sp, 8);
  const int auto_sp =
      operation.addressing == Addressing::indexed && operation.indexed.base == IndexBase::sp
          ? operation.indexed.update
          : 0;
  add_word(Reg::x9, stack, auto_sp - 2);
  add_word(Reg::x0, pc, operation.length);
  data_access({.word = true, .write = true});
  commit_index(operation);
  add_word(stack, stack, -2);
  code_.load(pc, Reg::sp, 8);
}
}
