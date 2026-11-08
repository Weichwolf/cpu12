// SPDX-License-Identifier: GPL-3.0-only
#include "jit/interrupt.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cpu12::detail::a64 {
namespace {
struct FramePart {
  Reg source;
  unsigned position;
  bool word = true;
};

constexpr unsigned frame_bytes = 9, frame_pivot = 8;
constexpr std::array frame_parts{FramePart{.source = pc, .position = 2},
                                 FramePart{.source = y, .position = 4},
                                 FramePart{.source = x, .position = 6},
                                 FramePart{.source = d, .position = 8},
                                 FramePart{.source = ccr, .position = 9, .word = false}};
}

Reg Emitter::frame_value(const Reg source,
                         const bool word,
                         const Byte next_pc,
                         const FrameOrder order) {
  auto value = source;
  if (source == pc) {
    if (order == FrameOrder::native) {
      code_.add(Reg::x0, pc, next_pc);
    } else {
      add_word(Reg::x0, pc, next_pc);
    }
    value = Reg::x0;
  } else {
    const auto dirty =
        code_.written_registers() & (std::uint32_t{1} << static_cast<unsigned>(source));
    if (dirty == 0) {
      constexpr std::array offsets{offsetof(Registers, d),
                                   offsetof(Registers, x),
                                   offsetof(Registers, y),
                                   offsetof(Registers, ccr)};
      constexpr std::array sources{d, x, y, ccr};
      const auto *const found = std::ranges::find(sources, source);
      const auto index = static_cast<std::size_t>(found - sources.begin());
      code_.load(Reg::x0, registers, offsets.at(index), word ? Access::half : Access::byte);
      value = Reg::x0;
    }
  }
  const bool reverse = order == FrameOrder::native ? source != d : source == d;
  if (word && reverse) {
    code_.reverse_half(Reg::x0, value);
    value = Reg::x0;
  }
  return value;
}

void Emitter::frame_ram(const Byte next_pc) {
  for (const auto part : frame_parts) {
    const auto value = frame_value(part.source, part.word, next_pc, FrameOrder::native);
    if (part.word) {
      code_.store(value, Reg::x11, frame_pivot - part.position, Access::half);
    } else {
      code_.subtract(Reg::x11, Reg::x11, 1, Width::x64);
      code_.store(value, Reg::x11, 0, Access::byte);
    }
  }
}

void Emitter::frame_admission(const Byte next_pc, Label &complete) {
  Label slow;
  code_.constant(Reg::x17, frame_bytes);
  code_.compare(stack, Reg::x17);
  code_.branch_far(Condition::no_carry, slow);
  auto regions = std::vector(memory_.regions().begin(), memory_.regions().end());
  std::ranges::sort(regions, [](const auto &left, const auto &right) {
    return left.bytes.size() > right.bytes.size();
  });
  for (const auto &region : regions) {
    if (!region.writable || !region.known.empty() || region.bytes.size() < frame_bytes) {
      continue;
    }
    Label next;
    code_.subtract(Reg::x9, stack, frame_bytes);
    code_.constant(Reg::x17, region.first);
    code_.alu(Alu::subtract, Reg::x9, Reg::x9, Reg::x17);
    code_.constant(Reg::x17, region.bytes.size() - frame_bytes);
    code_.compare(Reg::x9, Reg::x17);
    code_.branch_far(Condition::above, next);
    code_.constant(Reg::x11, reinterpret_cast<std::uintptr_t>(region.bytes.data()));
    code_.add(Reg::x9, Reg::x9, 1);
    code_.alu(Alu::add, Reg::x11, Reg::x11, Reg::x9, Width::x64);
    frame_ram(next_pc);
    code_.branch(complete);
    code_.bind(next);
  }
  code_.bind(slow);
}

void Emitter::stack_context(const Byte next_pc) {
  Label complete;
  frame_admission(next_pc, complete);
  for (const auto part : frame_parts) {
    add_word(Reg::x9, stack, -static_cast<int>(part.position));
    const auto value = frame_value(part.source, part.word, next_pc);
    if (value != Reg::x0) { code_.move(Reg::x0, value, Width::w32); }
    data_access({.word = part.word, .write = true});
  }
  code_.bind(complete);
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
