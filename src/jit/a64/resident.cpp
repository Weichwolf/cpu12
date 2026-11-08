// SPDX-License-Identifier: GPL-3.0-only
#include "jit/resident.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"
#include "jit/block.hpp"
#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cpu12::detail::a64 {
namespace {
constexpr unsigned location_offset = 24;
constexpr auto progress_offset =
    offsetof(NativeContext, progress) + offsetof(Progress, estimated_cycles);

unsigned base_cycles(const ResidentPlan &plan) {
  unsigned result = 0;
  for (const auto &operation : plan.operations) {
    result += instruction_timing(operation.instruction).base;
  }
  return result;
}
}

void Emitter::resident_grant(const ResidentPlan &plan,
                             const std::span<const BlockInstruction> suffix,
                             Label &general) {
  code_.constant(Reg::x11, 1);
  if (plan.loop) {
    code_.load(Reg::x11, Reg::x0, plan.counter_offset, Access::half);
    if (plan.increment) {
      code_.constant(Reg::x17, word_mask + 1U);
      code_.alu(Alu::subtract, Reg::x11, Reg::x17, Reg::x11);
    } else {
      code_.constant(Reg::x17, word_mask + 1U);
      code_.compare(Reg::x11, Reg::zr);
      code_.select(Reg::x11, Reg::x17, Reg::x11, Condition::equal);
    }
  }
  code_.constant(Reg::x17, plan.operations.size());
  code_.multiply(Reg::x12, Reg::x11, Reg::x17);
  code_.constant(Reg::x17, suffix.size());
  code_.alu(Alu::add, Reg::x9, Reg::x12, Reg::x17);
  code_.compare(Reg::x2, Reg::x9);
  code_.branch_far(Condition::no_carry, general);
  const auto extra = instruction_timing(plan.operations.back().instruction).taken_extra;
  code_.constant(Reg::x17, base_cycles(plan) + extra);
  code_.multiply(Reg::x13, Reg::x11, Reg::x17, Width::x64);
  if (plan.loop && extra != 0) {
    code_.constant(Reg::x17, extra);
    code_.alu(Alu::subtract, Reg::x13, Reg::x13, Reg::x17, Width::x64);
  }
  code_.load(Reg::x14, Reg::x1, progress_offset, Access::doubleword);
  code_.load(Reg::x9, Reg::x1, offsetof(NativeContext, cycle_deadline), Access::doubleword);
  code_.compare(Reg::x9, Reg::x14, Width::x64);
  code_.branch_far(Condition::no_carry, general);
  code_.alu(Alu::subtract, Reg::x9, Reg::x9, Reg::x14, Width::x64);
  unsigned suffix_cycles = 0;
  for (const auto &operation : suffix) {
    const auto timing = instruction_timing(operation.instruction);
    suffix_cycles += timing.base + timing.taken_extra;
  }
  code_.constant(Reg::x17, suffix_cycles);
  code_.alu(Alu::add, Reg::x10, Reg::x13, Reg::x17, Width::x64);
  code_.compare(Reg::x9, Reg::x10, Width::x64);
  code_.branch_far(Condition::no_carry, general);
}

void Emitter::resident_operand(const ResidentOperation &operation) {
  const auto &instruction = operation.instruction;
  if (operation.pointer == nullptr) {
    code_.constant(Reg::x1, instruction.operand);
    return;
  }
  code_.constant(Reg::x15, reinterpret_cast<std::uintptr_t>(operation.pointer));
  code_.load(Reg::x1, Reg::x15, 0, instruction.word ? Access::half : Access::byte);
  if (instruction.word) { code_.reverse_half(Reg::x1, Reg::x1); }
}

void Emitter::resident_operation(const ResidentOperation &operation, const bool live) {
  const auto &instruction = operation.instruction;
  const auto kind = instruction.kind;
  const auto changed = live ? operation.flags : 0;
  if (kind == Kind::nop || kind == Kind::branch) { return; }
  if (kind == Kind::loop || (kind == Kind::register_operation &&
                             (instruction.operation == RegisterOp::index_increment ||
                              instruction.operation == RegisterOp::index_decrement))) {
    const bool increment = instruction.operation == RegisterOp::increment ||
                           instruction.operation == RegisterOp::index_increment;
    add_word(word_target(instruction.target), word_target(instruction.target), increment ? 1 : -1);
    code_.move(Reg::x2, word_target(instruction.target), Width::w32);
  } else if (kind == Kind::multiply_divide) {
    math(instruction);
    return;
  } else if (kind == Kind::register_operation) {
    load_target(Reg::x0, instruction);
    code_.shift(Shift::left, Reg::x2, Reg::x0, 1);
    store_target(Reg::x2, instruction);
  } else if (kind == Kind::store) {
    load_target(Reg::x2, instruction);
    code_.constant(Reg::x15, reinterpret_cast<std::uintptr_t>(operation.pointer));
    if (instruction.word) { code_.reverse_half(Reg::x3, Reg::x2); }
    code_.store(instruction.word ? Reg::x3 : Reg::x2,
                Reg::x15,
                0,
                instruction.word ? Access::half : Access::byte);
  } else {
    if ((kind == Kind::compare || kind == Kind::bit_test) && changed == 0) { return; }
    resident_calculate(operation);
  }
  if (changed != 0) { resident_flags(operation); }
}

void Emitter::resident_calculate(const ResidentOperation &operation) {
  const auto &instruction = operation.instruction;
  const auto kind = instruction.kind;
  resident_operand(operation);
  load_target(Reg::x0, instruction);
  if (kind == Kind::load) {
    code_.move(Reg::x2, Reg::x1, Width::w32);
  } else {
    auto alu = Alu::bit_and;
    if (kind == Kind::add) { alu = Alu::add; }
    if (kind == Kind::subtract || kind == Kind::compare) { alu = Alu::subtract; }
    if (kind == Kind::bit_or) { alu = Alu::bit_or; }
    if (kind == Kind::bit_xor) { alu = Alu::bit_xor; }
    code_.alu(alu, Reg::x2, Reg::x0, Reg::x1);
  }
  if (kind != Kind::compare && kind != Kind::bit_test) { store_target(Reg::x2, instruction); }
}

void Emitter::resident_flags(const ResidentOperation &operation) {
  const auto &instruction = operation.instruction;
  const auto kind = instruction.kind;
  const auto changed = operation.flags;
  const bool subtract = kind == Kind::subtract || kind == Kind::compare;
  const bool index =
      kind == Kind::register_operation && (instruction.operation == RegisterOp::index_increment ||
                                           instruction.operation == RegisterOp::index_decrement);
  flags(instruction.word || index ? word_bits : byte_bits,
        {.changed = changed, .subtract = subtract, .arithmetic = subtract || kind == Kind::add});
  if (kind == Kind::register_operation && instruction.operation == RegisterOp::arithmetic_left &&
      (changed & flag_v) != 0) {
    code_.extract(Reg::x5, Reg::x2, word_bits - 1, 1);
    code_.extract(Reg::x6, Reg::x2, word_bits, 1);
    code_.alu(Alu::bit_xor, Reg::x5, Reg::x5, Reg::x6);
    code_.shift(Shift::left, Reg::x5, Reg::x5, 1);
    code_.alu(Alu::bit_or, Reg::x4, Reg::x4, Reg::x5);
  }
  code_.move(ccr, Reg::x4, Width::w32);
}

void Emitter::resident_body(const ResidentPlan &plan, const bool live) {
  for (const auto &operation : plan.operations) { resident_operation(operation, live); }
}

std::vector<Byte> Emitter::resident(const ResidentPlan &plan,
                                    const std::span<const BlockInstruction> suffix,
                                    const std::span<const Byte> general) {
  Label fallback;
  resident_grant(plan, suffix, fallback);
  begin(true);
  if (plan.loop) {
    Label repeat;
    Label final;
    code_.constant(Reg::x17, 1);
    code_.alu(Alu::subtract, Reg::x11, Reg::x11, Reg::x17);
    code_.branch_zero_far(Reg::x11, final);
    code_.bind(repeat);
    resident_body(plan, false);
    code_.constant(Reg::x17, 1);
    code_.alu(Alu::subtract, Reg::x11, Reg::x11, Reg::x17);
    code_.branch_zero_far(Reg::x11, repeat, Width::w32, true);
    code_.bind(final);
  }
  resident_body(plan, true);
  const auto &last = plan.operations.back();
  code_.constant(pc, static_cast<Address>(last.pc + last.instruction.length));
  if (!plan.loop && last.instruction.kind == Kind::branch && last.instruction.condition != 1) {
    if (last.instruction.condition == 0) {
      advance(last.instruction.operand);
    } else {
      Label not_taken;
      branch_guard(last.instruction, not_taken);
      advance(last.instruction.operand);
      code_.bind(not_taken);
    }
  }
  code_.move(retired, Reg::x12, Width::w32);
  code_.alu(Alu::subtract, remaining, remaining, retired);
  if (!plan.loop) {
    code_.constant(Reg::x17, instruction_timing(last.instruction).taken_extra);
    code_.alu(Alu::subtract, Reg::x13, Reg::x13, Reg::x17, Width::x64);
  }
  code_.alu(Alu::add, Reg::x14, Reg::x14, Reg::x13, Width::x64);
  code_.store(Reg::x14, context, progress_offset, Access::doubleword);
  if (!plan.loop) { control_clock(last.instruction); }
  code_.constant(Reg::x17,
                 std::uint32_t{last.pc} |
                     (std::uint32_t{last.instruction.opcode} << native_opcode_shift));
  code_.store(Reg::x17, Reg::sp, location_offset);
  if (!suffix.empty()) { sequence(suffix); }
  static_cast<void>(finish());
  code_.bind(fallback);
  code_.append(general);
  return code_.bytes();
}
}
