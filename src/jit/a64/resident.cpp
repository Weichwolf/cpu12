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

Alu resident_alu(const Kind kind) {
  if (kind == Kind::add) { return Alu::add; }
  if (kind == Kind::subtract || kind == Kind::compare) { return Alu::subtract; }
  if (kind == Kind::bit_or) { return Alu::bit_or; }
  if (kind == Kind::bit_xor) { return Alu::bit_xor; }
  return Alu::bit_and;
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

Reg Emitter::resident_word(const ResidentOperation &operation) const {
  if (!operation.instruction.word || operation.pointer == nullptr) { return Reg::zr; }
  const auto found = std::ranges::find(resident_words_, operation.pointer);
  if (found == resident_words_.end()) { return Reg::zr; }
  return region_values.at(static_cast<std::size_t>(found - resident_words_.begin()));
}

void Emitter::resident_value(const Reg value, const unsigned slot, const bool read) {
  constexpr std::array cache{Reg::x7, Reg::x8, Reg::x9, Reg::x10, Reg::x11};
  if (slot <= resident_gpr_slots_) {
    const auto cached = cache.at(slot - 1);
    code_.move(read ? value : cached, read ? cached : value, Width::w32);
  } else {
    constexpr unsigned vector_slot_bias = 15;
    code_.scalar_move(value, slot + vector_slot_bias, !read);
  }
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
  const auto cached = resident_word(operation);
  if (cached != Reg::zr) {
    code_.move(value, cached, Width::w32);
  } else if (instruction.addressing == Addressing::indexed) {
    code_.load(value, Reg::x7, 0, Access::half);
    code_.reverse_half(value, value);
    code_.add(Reg::x7, Reg::x7, 2, Width::x64);
    if (!resident_index_elided_) { add_word(x, x, 2); }
  } else if (operation.cached_operand) {
    code_.move(value, Reg::x8, Width::w32);
  } else if (operation.value_slot != 0 && !operation.fill_value) {
    resident_value(value, operation.value_slot, true);
  } else if (operation.forwarded_word != 0) {
    constexpr std::array words{Reg::zr, x, y, d, Reg::zr, stack};
    code_.extract(value, words.at(operation.forwarded_word), operation.forwarded_shift, word_bits);
  } else if (operation.pointer == nullptr) {
    code_.constant(value, instruction.operand);
  } else {
    resident_memory(value, operation, false);
    if (instruction.word) { code_.reverse_half(value, value); }
  }
  if (cached == Reg::zr && operation.value_slot != 0 && operation.fill_value) {
    resident_value(value, operation.value_slot, false);
  }
}

void Emitter::resident_multiply(const ResidentOperation &operation,
                                const bool live,
                                const bool square) {
  d_word_ = false;
  const bool sign = operation.instruction.math == MathOp::multiply_signed;
  code_.extract(Reg::x0, d, 0, word_bits, Width::w32, sign);
  if (!square) { code_.extract(Reg::x1, y, 0, word_bits, Width::w32, sign); }
  code_.multiply(d, Reg::x0, square ? Reg::x0 : Reg::x1);
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
    auto source = d;
    if (changed != 0 || !d_word_) {
      load_target(Reg::x0, instruction);
      source = Reg::x0;
    }
    code_.shift(Shift::left, Reg::x2, source, 1);
    store_target(Reg::x2, instruction);
    d_word_ = true;
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
      code_.move(Reg::x2, resident_counter_, Width::w32);
      resident_flags(operation);
    }
    return;
  }
  add_word(word_target(instruction.target), word_target(instruction.target), increment ? 1 : -1);
  if (changed != 0) {
    code_.move(Reg::x2, word_target(instruction.target), Width::w32);
    resident_flags(operation);
  }
}

void Emitter::resident_store(const ResidentOperation &operation) {
  const auto &instruction = operation.instruction;
  auto value = Reg::x2;
  const bool sum = resident_sum_live_ && instruction.word && instruction.target == WordTarget::d;
  if (sum) {
    value = Reg::x8;
    if (operation.flags != 0) { code_.move(Reg::x2, value, Width::w32); }
  } else if (instruction.word && operation.flags == 0 &&
             ((operation.value_slot == 0 && !operation.cache_store) ||
              instruction.target != WordTarget::d || d_word_)) {
    value = word_target(instruction.target);
  } else {
    load_target(value, instruction);
  }
  if (instruction.word) { code_.reverse_half(Reg::x3, value); }
  resident_memory(instruction.word ? Reg::x3 : value, operation, true);
  if (operation.cache_store && value != Reg::x8) { code_.move(Reg::x8, value, Width::w32); }
  const auto cached = resident_word(operation);
  if (cached != Reg::zr) {
    if (value != d || d_word_) {
      code_.move(cached, value, Width::w32);
    } else {
      code_.extract(cached, value, 0, word_bits);
    }
  } else if (operation.value_slot != 0 && operation.fill_value) {
    resident_value(value, operation.value_slot, false);
  }
}

bool Emitter::resident_fast(const ResidentOperation &operation) {
  if (operation.flags != 0) { return false; }
  const auto &instruction = operation.instruction;
  const auto kind = instruction.kind;
  if (kind == Kind::load && instruction.word) {
    resident_operand(operation, word_target(instruction.target));
    if (instruction.target == WordTarget::d) { d_word_ = true; }
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
    auto operand = resident_word(operation);
    if (operand == Reg::zr) {
      resident_operand(operation);
      operand = Reg::x1;
    }
    code_.alu(kind == Kind::add ? Alu::add : Alu::subtract, Reg::x2, target, operand);
  }
  store_target(Reg::x2, instruction);
  if (instruction.word && instruction.target == WordTarget::d) { d_word_ = true; }
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
    code_.alu(resident_alu(kind), Reg::x2, Reg::x0, Reg::x1);
  }
  if (kind != Kind::compare && kind != Kind::bit_test) {
    store_target(Reg::x2, instruction);
    if (instruction.word && instruction.target == WordTarget::d) { d_word_ = true; }
  }
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
  d_word_ = false;
  for (std::size_t index = 0; index < plan.operations.size(); ++index) {
    const auto &operation = plan.operations[index];
    if (resident_square_input(plan, index)) {
      resident_multiply(plan.operations[++index], live, true);
      continue;
    }
    if (!operation.initial_load) {
      resident_operation(operation, live);
    } else if (resident_word(operation) == Reg::zr && operation.value_slot != 0 &&
               operation.fill_value) {
      load_target(Reg::x1, operation.instruction);
      resident_value(Reg::x1, operation.value_slot, false);
    }
  }
}

void Emitter::resident_prepare(const ResidentPlan &plan) {
  resident_sum_ = accumulation(plan);
  if (plan.loop) {
    const auto word = plan.counter_offset == offsetof(Registers, x) ? 1U : 2U;
    if (independent_counter(plan, word)) { resident_counter_ = word == 1 ? x : y; }
  }
  if (resident_counter_ != Reg::zr) {
    code_.alu(
        plan.increment ? Alu::add : Alu::subtract, resident_counter_, resident_counter_, Reg::x11);
    code_.extract(resident_counter_, resident_counter_, 0, word_bits);
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
                                    const std::span<const Byte> general,
                                    const ResidentMode mode,
                                    const std::span<const BlockInstruction> terminal) {
  resident_gpr_slots_ = plan.loop ? 0U : 5U;
  Label fallback;
  if (mode == ResidentMode::bounded) {
    resident_bounded_grant(plan, fallback);
  } else {
    resident_grant(plan, terminal.empty() ? suffix : terminal, fallback);
  }
  begin(false);
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
  resident_body(plan, true);
  if (resident_sum_live_) { code_.move(d, Reg::x8, Width::w32); }
  if (resident_index_elided_) {
    code_.alu(Alu::add, x, x, Reg::x10);
    code_.extract(x, x, 0, word_bits);
  }
  const auto &last = plan.operations.back();
  code_.constant(pc,
                 mode == ResidentMode::bounded
                     ? plan.operations.front().pc
                     : static_cast<Address>(last.pc + last.instruction.length));
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
  if (!plan.loop) {
    code_.constant(Reg::x17, instruction_timing(last.instruction).taken_extra);
    code_.alu(Alu::subtract, Reg::x13, Reg::x13, Reg::x17, Width::x64);
  }
  code_.alu(Alu::add, Reg::x14, Reg::x14, Reg::x13, Width::x64);
  code_.store(Reg::x14, context, progress_offset, Access::doubleword);
  if (!plan.loop) { control_clock(last.instruction); }
  const auto location = mode == ResidentMode::bounded && plan.backedge_instructions != 0
                            ? plan.backedge_exit
                            : ExitLocation{.pc = last.pc, .opcode = last.instruction.opcode};
  code_.constant(Reg::x17,
                 std::uint32_t{location.pc} |
                     (std::uint32_t{location.opcode} << native_opcode_shift));
  code_.store(Reg::x17, Reg::sp, location_offset);
  if (swapped_d_) { code_.reverse_half(d, d); }
  swapped_d_ = false;
  if (!suffix.empty()) { sequence(suffix, Admission::reserved); }
  if (!terminal.empty()) { sequence(terminal, Admission::reserved); }
  static_cast<void>(finish());
  code_.bind(fallback);
  code_.append(general);
  return code_.bytes();
}
}
