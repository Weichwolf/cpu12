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

#include <algorithm>
#include <array>
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

bool independent_counter(const ResidentPlan &plan, const unsigned word) {
  return std::ranges::all_of(plan.operations, [word](const auto &operation) {
    const auto &instruction = operation.instruction;
    return instruction.kind == Kind::nop || instruction.kind == Kind::branch ||
           instruction.kind == Kind::loop || instruction.kind == Kind::register_operation ||
           native_word(instruction) != word;
  });
}

const ResidentOperation *accumulation(const ResidentPlan &plan) {
  if (!plan.loop) { return nullptr; }
  constexpr unsigned word_d = 3;
  const auto sum = std::ranges::find_if(
      plan.operations, [](const auto &operation) { return operation.cached_operand; });
  if (sum == plan.operations.end() || sum->instruction.kind != Kind::add ||
      !sum->instruction.word || native_word(sum->instruction) != word_d) {
    return nullptr;
  }
  const auto first = std::ranges::find_if(plan.operations, [](const auto &operation) {
    const auto kind = operation.instruction.kind;
    return kind != Kind::nop && kind != Kind::branch &&
           native_word(operation.instruction) == word_d;
  });
  if (first == sum || first->instruction.kind != Kind::load || !first->instruction.word ||
      first->initial_load) {
    return nullptr;
  }
  const auto tail = std::span(plan.operations)
                        .subspan(static_cast<std::size_t>(sum - plan.operations.begin()) + 1);
  const bool stores = std::ranges::all_of(tail, [](const auto &operation) {
    const auto &instruction = operation.instruction;
    if (instruction.kind == Kind::nop || instruction.kind == Kind::branch) { return true; }
    if (native_word(instruction) != word_d) { return !operation.cache_store; }
    return instruction.kind == Kind::store && instruction.word && operation.value_slot == 0;
  });
  const bool retained =
      std::ranges::any_of(tail, [](const auto &operation) { return operation.cache_store; });
  return stores && retained ? &*sum : nullptr;
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
  code_.constant(Reg::x17, plan.operations.size() + plan.backedge_instructions);
  code_.multiply(Reg::x12, Reg::x11, Reg::x17);
  if (plan.backedge_instructions != 0) {
    code_.subtract(Reg::x12, Reg::x12, static_cast<std::uint16_t>(plan.backedge_instructions));
  }
  code_.constant(Reg::x17, suffix.size());
  code_.alu(Alu::add, Reg::x9, Reg::x12, Reg::x17);
  code_.compare(Reg::x2, Reg::x9);
  code_.branch_far(Condition::no_carry, general);
  const auto extra = instruction_timing(plan.operations.back().instruction).taken_extra;
  const auto repeat_extra = plan.backedge_instructions != 0 ? plan.backedge_cycles : extra;
  code_.constant(Reg::x17, base_cycles(plan) + repeat_extra);
  code_.multiply(Reg::x13, Reg::x11, Reg::x17, Width::x64);
  if (plan.loop && extra != 0) {
    code_.constant(Reg::x17, repeat_extra);
    code_.alu(Alu::subtract, Reg::x13, Reg::x13, Reg::x17, Width::x64);
    if (plan.backedge_instructions != 0) {
      code_.add(Reg::x13, Reg::x13, static_cast<std::uint16_t>(extra), Width::x64);
    }
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
  if (!plan.indexed_regions.empty()) { resident_indexed_grant(plan, general); }
}

void Emitter::resident_indexed_grant(const ResidentPlan &plan, Label &general) {
  code_.load(Reg::x6, Reg::x0, offsetof(Registers, x), Access::half);
  code_.shift(Shift::left, Reg::x5, Reg::x11, 1);
  code_.alu(Alu::add, Reg::x5, Reg::x5, Reg::x6);
  Label admitted;
  for (const auto &region : plan.indexed_regions) {
    Label next;
    code_.constant(Reg::x17, region.first);
    code_.compare(Reg::x6, Reg::x17);
    code_.branch_far(Condition::no_carry, next);
    code_.constant(Reg::x17, std::size_t{region.first} + region.bytes.size());
    code_.compare(Reg::x5, Reg::x17);
    code_.branch_far(Condition::above, next);
    code_.constant(Reg::x17, region.first);
    code_.alu(Alu::subtract, Reg::x9, Reg::x6, Reg::x17);
    code_.constant(Reg::x7, reinterpret_cast<std::uintptr_t>(region.bytes.data()));
    code_.alu(Alu::add, Reg::x7, Reg::x7, Reg::x9, Width::x64);
    code_.branch(admitted);
    code_.bind(next);
  }
  code_.branch(general);
  code_.bind(admitted);
}

void Emitter::resident_memory(const Reg value,
                              const ResidentOperation &operation,
                              const bool write) {
  const auto address = reinterpret_cast<std::uintptr_t>(operation.pointer);
  const auto base = reinterpret_cast<std::uintptr_t>(resident_base_);
  const auto scale = operation.instruction.word ? 2U : 1U;
  constexpr std::uintptr_t maximum_offset = 4095;
  const bool relative =
      address >= base && address - base <= maximum_offset * scale && (address - base) % scale == 0;
  auto pointer = Reg::x16;
  const std::size_t offset = relative ? static_cast<std::size_t>(address - base) : 0;
  if (!relative) {
    pointer = Reg::x15;
    code_.constant(pointer, address);
  }
  const auto access = operation.instruction.word ? Access::half : Access::byte;
  if (write) {
    code_.store(value, pointer, offset, access);
  } else {
    code_.load(value, pointer, offset, access);
  }
}

void Emitter::resident_operand(const ResidentOperation &operation, const Reg value) {
  const auto &instruction = operation.instruction;
  constexpr unsigned vector_slot_bias = 15;
  if (instruction.addressing == Addressing::indexed) {
    code_.load(value, Reg::x7, 0, Access::half);
    code_.reverse_half(value, value);
    code_.add(Reg::x7, Reg::x7, 2, Width::x64);
    if (!resident_index_elided_) { add_word(x, x, 2); }
  } else if (operation.cached_operand) {
    code_.move(value, Reg::x8, Width::w32);
  } else if (operation.value_slot != 0 && !operation.fill_value) {
    code_.scalar_move(value, operation.value_slot + vector_slot_bias, false);
  } else if (operation.forwarded_word != 0) {
    constexpr std::array words{Reg::zr, x, y, d, Reg::zr, stack};
    code_.extract(value, words.at(operation.forwarded_word), operation.forwarded_shift, word_bits);
  } else if (operation.pointer == nullptr) {
    code_.constant(value, instruction.operand);
  } else {
    resident_memory(value, operation, false);
    if (instruction.word) { code_.reverse_half(value, value); }
  }
  if (operation.value_slot != 0 && operation.fill_value) {
    code_.scalar_move(value, operation.value_slot + vector_slot_bias, true);
  }
}

void Emitter::resident_multiply(const ResidentOperation &operation, const bool live) {
  const bool sign = operation.instruction.math == MathOp::multiply_signed;
  code_.extract(Reg::x0, d, 0, word_bits, Width::w32, sign);
  code_.extract(Reg::x1, y, 0, word_bits, Width::w32, sign);
  code_.multiply(d, Reg::x0, Reg::x1);
  code_.extract(y, d, word_bits, word_bits);
  const auto changed = live ? operation.flags : 0;
  if (changed == 0) { return; }
  code_.move(Reg::x2, d, Width::w32);
  flags(2 * word_bits, {.changed = changed & ~flag_c});
  if ((changed & flag_c) != 0) {
    mask(Reg::x4, Reg::x4, byte_mask ^ flag_c);
    code_.extract(Reg::x5, d, word_bits - 1, 1);
    code_.alu(Alu::bit_or, Reg::x4, Reg::x4, Reg::x5);
  }
  code_.move(ccr, Reg::x4, Width::w32);
}

void Emitter::resident_operation(const ResidentOperation &operation, const bool live) {
  const auto &instruction = operation.instruction;
  const auto kind = instruction.kind;
  const auto changed = live ? operation.flags : 0;
  if (kind == Kind::nop || kind == Kind::branch) { return; }
  if (&operation == resident_sum_) {
    resident_accumulate(operation, live);
    return;
  }
  if (kind == Kind::loop || (kind == Kind::register_operation &&
                             (instruction.operation == RegisterOp::index_increment ||
                              instruction.operation == RegisterOp::index_decrement))) {
    resident_update(operation, live);
    return;
  }
  if (kind == Kind::multiply_divide) {
    resident_multiply(operation, live);
    return;
  }
  if (kind == Kind::register_operation) {
    load_target(Reg::x0, instruction);
    code_.shift(Shift::left, Reg::x2, Reg::x0, 1);
    store_target(Reg::x2, instruction);
  } else if (kind == Kind::store) {
    auto value = operation;
    if (!live) { value.flags = 0; }
    resident_store(value);
  } else {
    if ((kind == Kind::compare || kind == Kind::bit_test) && changed == 0) { return; }
    auto value = operation;
    if (!live) { value.flags = 0; }
    resident_calculate(value);
  }
  if (changed != 0) { resident_flags(operation); }
}

void Emitter::resident_accumulate(const ResidentOperation &operation, const bool live) {
  const auto changed = live ? operation.flags : 0;
  if (changed != 0) {
    load_target(Reg::x0, operation.instruction);
    code_.move(Reg::x1, Reg::x8, Width::w32);
  }
  code_.alu(Alu::add, Reg::x2, changed != 0 ? Reg::x0 : d, Reg::x8);
  code_.extract(Reg::x8, Reg::x2, 0, word_bits);
  resident_sum_live_ = true;
  if (changed != 0) { resident_flags(operation); }
}

void Emitter::resident_update(const ResidentOperation &operation, const bool live) {
  const auto &instruction = operation.instruction;
  const auto changed = live ? operation.flags : 0;
  const bool increment = instruction.operation == RegisterOp::increment ||
                         instruction.operation == RegisterOp::index_increment;
  if (word_target(instruction.target) == resident_counter_) {
    if (changed != 0) {
      code_.constant(Reg::x2, 0);
      resident_flags(operation);
    }
    return;
  }
  add_word(word_target(instruction.target), word_target(instruction.target), increment ? 1 : -1);
  code_.move(Reg::x2, word_target(instruction.target), Width::w32);
  if (changed != 0) { resident_flags(operation); }
}

void Emitter::resident_store(const ResidentOperation &operation) {
  const auto &instruction = operation.instruction;
  auto value = Reg::x2;
  const bool sum = resident_sum_live_ && instruction.word && instruction.target == WordTarget::d;
  if (sum) {
    value = Reg::x8;
    if (operation.flags != 0) { code_.move(Reg::x2, value, Width::w32); }
  } else if (instruction.word && operation.flags == 0 && operation.value_slot == 0 &&
             !operation.cache_store) {
    value = word_target(instruction.target);
  } else {
    load_target(value, instruction);
  }
  if (instruction.word) { code_.reverse_half(Reg::x3, value); }
  resident_memory(instruction.word ? Reg::x3 : value, operation, true);
  if (operation.cache_store && value != Reg::x8) { code_.move(Reg::x8, value, Width::w32); }
  if (operation.value_slot != 0 && operation.fill_value) {
    constexpr unsigned vector_slot_bias = 15;
    code_.scalar_move(value, operation.value_slot + vector_slot_bias, true);
  }
}

bool Emitter::resident_fast(const ResidentOperation &operation) {
  if (operation.flags != 0) { return false; }
  const auto &instruction = operation.instruction;
  const auto kind = instruction.kind;
  if (kind == Kind::load && instruction.word) {
    resident_operand(operation, word_target(instruction.target));
    return true;
  }
  const bool low = instruction.word || instruction.accumulator_b != swapped_d_;
  if (!low || (kind != Kind::add && kind != Kind::subtract)) { return false; }
  const auto target = instruction.word ? word_target(instruction.target) : d;
  constexpr unsigned maximum_immediate = 4095;
  if (instruction.addressing == Addressing::immediate && instruction.operand <= maximum_immediate) {
    if (kind == Kind::add) {
      code_.add(Reg::x2, target, instruction.operand);
    } else {
      code_.subtract(Reg::x2, target, instruction.operand);
    }
  } else {
    resident_operand(operation);
    code_.alu(kind == Kind::add ? Alu::add : Alu::subtract, Reg::x2, target, Reg::x1);
  }
  store_target(Reg::x2, instruction);
  return true;
}

void Emitter::resident_calculate(const ResidentOperation &operation) {
  const auto &instruction = operation.instruction;
  const auto kind = instruction.kind;
  if (resident_fast(operation)) { return; }
  resident_operand(operation);
  const bool byte_low = !instruction.word && instruction.accumulator_b != swapped_d_;
  if (kind != Kind::load) {
    if (operation.flags == 0 && (instruction.word || byte_low)) {
      code_.move(Reg::x0, instruction.word ? word_target(instruction.target) : d, Width::w32);
    } else {
      load_target(Reg::x0, instruction);
    }
  }
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
  resident_sum_live_ = false;
  for (const auto &operation : plan.operations) {
    if (!operation.initial_load) { resident_operation(operation, live); }
  }
}

void Emitter::resident_prepare(const ResidentPlan &plan) {
  resident_sum_ = accumulation(plan);
  if (plan.loop) {
    const auto word = plan.counter_offset == offsetof(Registers, x) ? 1U : 2U;
    if (independent_counter(plan, word)) { resident_counter_ = word == 1 ? x : y; }
  }
  resident_index_elided_ = !plan.indexed_regions.empty() && independent_counter(plan, 1);
  if (resident_index_elided_) { code_.shift(Shift::left, Reg::x10, Reg::x11, 1); }
  swapped_d_ = plan.swapped_d;
  if (swapped_d_) { code_.reverse_half(d, d); }
  resident_base_ = plan.base;
  for (const auto &operation : plan.operations) {
    if (operation.pointer != nullptr && reinterpret_cast<std::uintptr_t>(operation.pointer) <
                                            reinterpret_cast<std::uintptr_t>(resident_base_)) {
      resident_base_ = operation.pointer;
    }
  }
  if (resident_base_ != nullptr) {
    code_.constant(Reg::x16, reinterpret_cast<std::uintptr_t>(resident_base_));
  }
  for (auto operation : plan.operations) {
    operation.flags = 0;
    if (operation.initial_load) { resident_operation(operation, false); }
    if (operation.cached_operand) {
      operation.cached_operand = false;
      resident_operand(operation);
      code_.move(Reg::x8, Reg::x1, Width::w32);
    }
  }
}

std::vector<Byte> Emitter::resident(const ResidentPlan &plan,
                                    const std::span<const BlockInstruction> suffix,
                                    const std::span<const Byte> general) {
  Label fallback;
  resident_grant(plan, suffix, fallback);
  begin(true);
  resident_prepare(plan);
  if (plan.loop) {
    Label repeat;
    Label final;
    code_.subtract(Reg::x11, Reg::x11, 1);
    code_.branch_zero_far(Reg::x11, final);
    code_.bind(repeat);
    resident_body(plan, false);
    code_.subtract(Reg::x11, Reg::x11, 1);
    code_.branch_zero_far(Reg::x11, repeat, Width::w32, true);
    code_.bind(final);
  }
  if (resident_counter_ != Reg::zr) { code_.constant(resident_counter_, 0); }
  resident_body(plan, true);
  if (resident_sum_live_) { code_.move(d, Reg::x8, Width::w32); }
  if (resident_index_elided_) {
    code_.alu(Alu::add, x, x, Reg::x10);
    code_.extract(x, x, 0, word_bits);
  }
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
  if (swapped_d_) { code_.reverse_half(d, d); }
  swapped_d_ = false;
  if (!suffix.empty()) { sequence(suffix); }
  static_cast<void>(finish());
  code_.bind(fallback);
  code_.append(general);
  return code_.bytes();
}
}
