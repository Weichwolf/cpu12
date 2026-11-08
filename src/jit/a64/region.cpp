// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"
#include "jit/block.hpp"
#include "jit/resident.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace cpu12::detail::a64 {
namespace {
constexpr auto progress_offset =
    offsetof(NativeContext, progress) + offsetof(Progress, estimated_cycles);

unsigned cycles(const ResidentPlan &plan) {
  unsigned result = 0;
  for (const auto &operation : plan.operations) {
    result += instruction_timing(operation.instruction).base;
  }
  return result;
}

enum class TailForm : std::uint8_t { comparison, counter, unconditional };

TailForm tail_form(const ResidentPlan &tail) {
  if (tail.operations.back().instruction.condition == 0) { return TailForm::unconditional; }
  return tail.operations[tail.operations.size() - 2].instruction.kind == Kind::compare
             ? TailForm::comparison
             : TailForm::counter;
}

Condition native_condition(const unsigned condition) {
  switch (condition) {
    case 4: return Condition::carry;
    case 5: return Condition::no_carry;
    case 6: return Condition::not_equal;
    case 7: return Condition::equal;
    default: throw std::logic_error("Unsupported ARM64 resident region guard");
  }
}

bool single_counter(const ResidentPlan &tail) {
  const auto &update = tail.operations[tail.operations.size() - 2].instruction;
  const auto word = 1U << native_word(update);
  return std::ranges::count_if(tail.operations, [word](const auto &operation) {
           return (written_words(operation.instruction) & word) != 0;
         }) == 1;
}

bool isolated_counter(const ResidentPlan &plan, const WordTarget target) {
  return std::ranges::all_of(plan.operations, [target](const auto &operation) {
    const auto &instruction = operation.instruction;
    const auto kind = instruction.kind;
    if (instruction.addressing == Addressing::indexed) { return false; }
    if (kind == Kind::multiply_divide) { return target != WordTarget::y; }
    return kind == Kind::nop || kind == Kind::branch || kind == Kind::loop ||
           kind == Kind::register_operation || instruction.target != target;
  });
}

std::unordered_set<std::uintptr_t> blocked_words(const ResidentPlan &plan) {
  std::unordered_set<std::uintptr_t> result;
  result.reserve(plan.operations.size() * 2);
  for (const auto &operation : plan.operations) {
    if (operation.instruction.kind != Kind::store) { continue; }
    const auto address = reinterpret_cast<std::uintptr_t>(operation.pointer);
    result.insert(address - 1);
    result.insert(address + (operation.instruction.word ? 1U : 0U));
  }
  return result;
}
}

void Emitter::resident_region_cache(const ResidentPlan &plan) {
  const auto blocked = blocked_words(plan);
  for (const auto &operation : plan.operations) {
    if (resident_words_.size() == region_values.size()) { break; }
    const auto address = reinterpret_cast<std::uintptr_t>(operation.pointer);
    if (!operation.instruction.word || operation.pointer == nullptr ||
        operation.instruction.kind == Kind::store || resident_word(operation) != Reg::zr ||
        blocked.contains(address)) {
      continue;
    }
    const auto target = region_values[resident_words_.size()];
    resident_words_.push_back(operation.pointer);
    const auto first =
        std::ranges::find(plan.operations, operation.pointer, &ResidentOperation::pointer);
    if (first->initial_load && first->instruction.word) {
      code_.extract(target, word_target(first->instruction.target), 0, word_bits, Width::w32, true);
    } else if (first->instruction.kind != Kind::store) {
      resident_memory(target, operation, false);
      code_.reverse_half(target, target);
      code_.extract(target, target, 0, word_bits, Width::w32, true);
    }
  }
}

void Emitter::resident_writeback(const RegionExit exit) {
  const auto store = [this](const std::span<const Byte *const> pointers) {
    for (std::size_t index = 0; index < pointers.size(); ++index) {
      const ResidentOperation operation{.instruction = {.kind = Kind::store, .word = true},
                                        .pointer = pointers[index]};
      const auto first = resident_word(operation);
      if (index + 1 < pointers.size() && resident_pair(pointers[index], pointers[index + 1])) {
        const ResidentOperation next{.instruction = {.word = true}, .pointer = pointers[++index]};
        code_.insert(first, resident_word(next), word_bits, word_bits);
        code_.reverse_half(Reg::x3, first);
        resident_memory(Reg::x3, operation, true, Access::word);
      } else {
        code_.reverse_half(Reg::x3, first);
        resident_memory(Reg::x3, operation, true);
      }
    }
  };
  store(head_stores_);
  Label complete;
  if (exit == RegionExit::head && !tail_stores_.empty()) {
    code_.branch_zero_far(Reg::x11, complete);
  }
  store(tail_stores_);
  code_.bind(complete);
}

Reg Emitter::resident_compare(const ResidentOperation &operation) {
  const auto &instruction = operation.instruction;
  auto source = Reg::x0;
  if (instruction.word && (instruction.target != WordTarget::d || d_word_)) {
    source = word_target(instruction.target);
  } else {
    load_target(source, instruction);
  }
  constexpr unsigned maximum_immediate = 4095;
  if (instruction.addressing == Addressing::immediate && instruction.operand <= maximum_immediate) {
    code_.compare(source, instruction.operand);
  } else {
    resident_operand(operation);
    code_.compare(source, Reg::x1);
  }
  return source;
}

void Emitter::resident_compare_flags(const ResidentOperation &operation, const Reg source) {
  resident_operand(operation);
  const auto shift = 32U - (operation.instruction.word ? word_bits : byte_bits);
  code_.shift(Shift::left, Reg::x0, source, shift);
  code_.shift(Shift::left, Reg::x1, Reg::x1, shift);
  code_.compare(Reg::x0, Reg::x1);
  constexpr std::array conditions{
      Condition::no_carry, Condition::overflow, Condition::equal, Condition::negative};
  for (unsigned bit = 0; bit < conditions.size(); ++bit) {
    if ((operation.flags & (1U << bit)) == 0) { continue; }
    code_.set(Reg::x5, conditions[bit]);
    code_.insert(ccr, Reg::x5, bit, 1);
  }
}

void Emitter::resident_region_exit(const ResidentRegion &region,
                                   const RegionExit exit,
                                   const Publication publication) {
  if (region_counter_ != Reg::zr && exit == RegionExit::head) {
    const auto &update = region.tail.operations[region.tail.operations.size() - 2].instruction;
    const auto operation =
        update.operation == RegisterOp::index_increment ? Alu::add : Alu::subtract;
    code_.alu(operation, Reg::x11, Reg::x11, region_counter_);
  }
  resident_writeback(exit);
  const auto count = region.head.operations.size() + region.tail.operations.size();
  const bool head = exit == RegionExit::head;
  const auto &last = head ? region.head.operations.back() : region.tail.operations.back();
  const auto extra = instruction_timing(last.instruction).taken_extra;
  const auto full_cycles =
      cycles(region.head) + cycles(region.tail) +
      instruction_timing(region.tail.operations.back().instruction).taken_extra;
  code_.multiply(retired, Reg::x11, count, Reg::x17);
  code_.multiply(Reg::x13, Reg::x11, full_cycles, Reg::x17, Width::x64);
  if (head) {
    code_.add(retired, retired, region.head.operations.size(), Reg::x17);
    code_.add(Reg::x13, Reg::x13, cycles(region.head) + extra, Reg::x17, Width::x64);
  } else if (exit == RegionExit::tail) {
    code_.subtract(Reg::x13, Reg::x13, static_cast<std::uint16_t>(extra), Width::x64);
  }
  code_.alu(Alu::add, Reg::x14, Reg::x14, Reg::x13, Width::x64);
  if (publication == Publication::immediate) {
    code_.store(Reg::x14, context, progress_offset, Access::doubleword);
  }
  const auto &tail = region.tail.operations.back();
  if (!terminal_exit_) {
    code_.constant(pc,
                   exit == RegionExit::budget
                       ? region.head.operations.front().pc
                       : static_cast<Address>(tail.pc + tail.instruction.length));
  }
  record_exit({.pc = last.pc, .opcode = last.instruction.opcode});
}

Reg Emitter::resident_region_tail(const ResidentRegion &region,
                                  RegionLabels &labels,
                                  const bool counted) {
  const auto &tail = region.tail;
  const auto &branch = tail.operations.back().instruction;
  const auto &guard = tail.operations[tail.operations.size() - 2];
  const auto form = tail_form(tail);
  if (form == TailForm::comparison) {
    auto prefix = tail;
    prefix.operations.resize(prefix.operations.size() - 2);
    resident_body(prefix, true);
    const auto source = resident_compare(guard);
    code_.add(Reg::x11, Reg::x11, 1);
    const auto inverted =
        static_cast<Condition>(static_cast<unsigned>(native_condition(branch.condition)) ^ 1U);
    code_.branch_far(inverted, labels.tail);
    code_.subtract(remaining, remaining, 1);
    code_.branch_zero_far(remaining, labels.budget);
    code_.branch(labels.repeat);
    return source;
  }
  if (form == TailForm::counter && !single_counter(tail)) {
    resident_body(tail, true);
    code_.add(Reg::x11, Reg::x11, 1);
    branch_guard(branch, labels.tail);
    code_.subtract(remaining, remaining, 1);
    code_.branch_zero_far(remaining, labels.budget);
    code_.branch(labels.repeat);
    return Reg::zr;
  }
  const bool increment = guard.instruction.operation == RegisterOp::index_increment;
  const auto limit = counted && increment ? word_mask : 1U;
  const auto counter = counted ? word_target(guard.instruction.target) : remaining;
  if (region_counter_ != Reg::zr && increment) {
    code_.add(Reg::x17, counter, 1);
    code_.branch_zero_far(Reg::x17, labels.final_tail);
  } else {
    if (limit == 1) {
      code_.compare(counter, 1);
    } else {
      code_.constant(Reg::x17, limit);
      code_.compare(counter, Reg::x17);
    }
    code_.branch_far(Condition::equal, labels.final_tail);
  }
  if (form == TailForm::counter && !counted) {
    const auto &update = guard.instruction;
    code_.constant(Reg::x17, update.operation == RegisterOp::index_increment ? word_mask : 1U);
    code_.compare(word_target(update.target), Reg::x17);
    code_.branch_far(branch.condition == 6 ? Condition::equal : Condition::not_equal,
                     labels.final_tail);
  }
  resident_body(tail, false);
  if (region_counter_ == Reg::zr) { code_.add(Reg::x11, Reg::x11, 1); }
  if (!counted) { code_.subtract(remaining, remaining, 1); }
  code_.branch(labels.repeat);
  code_.bind(labels.final_tail);
  resident_body(tail, true);
  if (region_counter_ == Reg::zr) { code_.add(Reg::x11, Reg::x11, 1); }
  if (counted) {
    code_.branch(labels.tail);
  } else {
    if (form == TailForm::counter) { branch_guard(branch, labels.tail); }
    code_.branch(labels.budget);
  }
  return Reg::zr;
}

std::vector<Byte> Emitter::resident_region(const ResidentRegion &region,
                                           const std::span<const Byte> general,
                                           const std::span<const BlockInstruction> terminal) {
  resident_inputs(region.head);
  auto combined = region.head;
  combined.operations.insert(
      combined.operations.end(), region.tail.operations.begin(), region.tail.operations.end());
  const auto form = tail_form(region.tail);
  const bool counted =
      !terminal.empty() && form == TailForm::counter &&
      region.tail.operations.back().instruction.condition == 6 && single_counter(combined) &&
      cycles(region.tail) >=
          instruction_timing(region.head.operations.back().instruction).taken_extra;
  resident_gpr_slots_ = 0;
  const auto &update = region.tail.operations[region.tail.operations.size() - 2].instruction;
  Label fallback;
  if (counted) {
    terminal_exit_ =
        ExitLocation{.pc = terminal.front().pc, .opcode = terminal.front().instruction.opcode};
    auto admission = combined;
    if (isolated_counter(combined, update.target)) {
      region_counter_ = word_target(update.target);
      input_registers_ &= ~(1U << static_cast<unsigned>(region_counter_));
    }
    admission.loop = true;
    admission.counter_offset =
        update.target == WordTarget::x ? offsetof(Registers, x) : offsetof(Registers, y);
    admission.increment = update.operation == RegisterOp::index_increment;
    resident_grant(admission, terminal, fallback);
  } else {
    resident_region_grant(region, terminal, fallback);
  }
  begin(EntryMode::resident);
  if (region_counter_ != Reg::zr) {
    if (update.operation == RegisterOp::index_increment) {
      code_.alu(Alu::subtract, region_counter_, Reg::zr, Reg::x11);
    } else {
      code_.move(region_counter_, Reg::x11, Width::w32);
    }
  }
  if (!counted) { code_.move(remaining, Reg::x11, Width::w32); }
  resident_prepare(combined);
  resident_region_cache(combined);
  resident_region_stores(combined, region.head.operations.size());
  if (region_counter_ == Reg::zr) { code_.constant(Reg::x11, 0); }
  auto prefix = region.head;
  prefix.operations.resize(prefix.operations.size() - 2);
  const auto &guard = region.head.operations[region.head.operations.size() - 2];
  const auto &branch = region.head.operations.back().instruction;
  RegionLabels labels;
  loop_offset_ = code_.size();
  code_.bind(labels.repeat);
  resident_body(prefix, true);
  const auto head_source = resident_compare(guard);
  code_.branch_far(native_condition(branch.condition), labels.head);
  const auto tail_source = resident_region_tail(region, labels, counted);
  code_.bind(labels.tail);
  if (form == TailForm::comparison) {
    resident_compare_flags(region.tail.operations[region.tail.operations.size() - 2], tail_source);
  }
  const auto publication = terminal.empty() ? Publication::immediate : Publication::deferred;
  resident_region_exit(region, RegionExit::tail, publication);
  code_.branch(labels.terminal);
  code_.bind(labels.head);
  resident_compare_flags(guard, head_source);
  resident_region_exit(region, RegionExit::head, publication);
  code_.branch(labels.terminal);
  if (!counted) {
    code_.bind(labels.budget);
    if (form == TailForm::comparison) {
      resident_compare_flags(region.tail.operations[region.tail.operations.size() - 2],
                             tail_source);
    }
    resident_region_exit(region, RegionExit::budget);
    if (swapped_d_) { code_.reverse_half(d, d); }
    code_.branch(labels.complete);
  }
  code_.bind(labels.terminal);
  if (swapped_d_) { code_.reverse_half(d, d); }
  swapped_d_ = false;
  resident_suffix(terminal);
  code_.bind(labels.complete);
  static_cast<void>(finish());
  code_.bind(fallback);
  code_.append(general);
  return code_.bytes();
}
}
