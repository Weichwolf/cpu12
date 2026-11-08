// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"
#include "jit/block.hpp"
#include "jit/resident.hpp"
#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace cpu12::detail::a64 {
namespace {
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

void Emitter::resident_trips(const ResidentPlan &plan) {
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
}

void Emitter::resident_grant(const ResidentPlan &plan,
                             const std::span<const BlockInstruction> suffix,
                             Label &general) {
  resident_trips(plan);
  code_.constant(Reg::x17, plan.operations.size() + plan.backedge_instructions);
  code_.multiply(Reg::x12, Reg::x11, Reg::x17);
  if (plan.backedge_instructions != 0) {
    code_.subtract(Reg::x12, Reg::x12, static_cast<std::uint16_t>(plan.backedge_instructions));
  }
  code_.constant(Reg::x17, suffix.size());
  code_.alu(Alu::add, Reg::x15, Reg::x12, Reg::x17);
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
  unsigned suffix_cycles = 0;
  for (const auto &operation : suffix) {
    const auto timing = instruction_timing(operation.instruction);
    suffix_cycles += timing.base + timing.taken_extra;
  }
  code_.constant(Reg::x17, suffix_cycles);
  code_.alu(Alu::add, Reg::x10, Reg::x13, Reg::x17, Width::x64);
  code_.alu(Alu::add_flags, Reg::x10, Reg::x14, Reg::x10, Width::x64);
  code_.compare_if(Reg::x2, Reg::x15, Condition::no_carry);
  code_.compare_if(Reg::x9, Reg::x10, Condition::carry, Width::x64);
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

void Emitter::resident_bounded_grant(const ResidentPlan &plan, Label &general) {
  resident_trips(plan);
  code_.subtract(Reg::x11, Reg::x11, 1);
  const auto count = plan.operations.size() + plan.backedge_instructions;
  const auto extra = plan.backedge_instructions != 0
                         ? plan.backedge_cycles
                         : instruction_timing(plan.operations.back().instruction).taken_extra;
  const auto cycles = base_cycles(plan) + extra;
  code_.constant(Reg::x17, count);
  code_.divide(Reg::x12, Reg::x2, Reg::x17, false);
  code_.compare(Reg::x12, Reg::x11);
  code_.select(Reg::x11, Reg::x11, Reg::x12, Condition::carry);
  code_.load(Reg::x14, Reg::x1, progress_offset, Access::doubleword);
  code_.load(Reg::x9, Reg::x1, offsetof(NativeContext, cycle_deadline), Access::doubleword);
  code_.compare(Reg::x9, Reg::x14, Width::x64);
  code_.branch_far(Condition::no_carry, general);
  code_.alu(Alu::subtract, Reg::x9, Reg::x9, Reg::x14, Width::x64);
  code_.constant(Reg::x17, cycles);
  code_.divide(Reg::x10, Reg::x9, Reg::x17, false, Width::x64);
  code_.compare(Reg::x10, Reg::x11, Width::x64);
  code_.select(Reg::x11, Reg::x11, Reg::x10, Condition::carry, Width::x64);
  code_.branch_zero_far(Reg::x11, general);
  code_.constant(Reg::x17, count);
  code_.multiply(Reg::x12, Reg::x11, Reg::x17);
  code_.constant(Reg::x17, cycles);
  code_.multiply(Reg::x13, Reg::x11, Reg::x17, Width::x64);
  if (!plan.indexed_regions.empty()) { resident_indexed_grant(plan, general); }
}

void Emitter::resident_region_grant(const ResidentRegion &region,
                                    const std::span<const BlockInstruction> terminal,
                                    Label &general) {
  const auto count = region.head.operations.size() + region.tail.operations.size();
  const auto cycles = base_cycles(region.head) + base_cycles(region.tail) +
                      instruction_timing(region.tail.operations.back().instruction).taken_extra;
  code_.constant(Reg::x17, terminal.size());
  code_.compare(Reg::x2, Reg::x17);
  code_.branch_far(Condition::no_carry, general);
  code_.alu(Alu::subtract, Reg::x9, Reg::x2, Reg::x17);
  code_.constant(Reg::x17, count);
  code_.divide(Reg::x11, Reg::x9, Reg::x17, false);
  code_.load(Reg::x14, Reg::x1, progress_offset, Access::doubleword);
  code_.load(Reg::x9, Reg::x1, offsetof(NativeContext, cycle_deadline), Access::doubleword);
  code_.compare(Reg::x9, Reg::x14, Width::x64);
  code_.branch_far(Condition::no_carry, general);
  code_.alu(Alu::subtract, Reg::x9, Reg::x9, Reg::x14, Width::x64);
  unsigned reserved = 0;
  for (const auto &operation : terminal) {
    const auto timing = instruction_timing(operation.instruction);
    reserved += timing.base + timing.taken_extra;
  }
  code_.constant(Reg::x17, reserved);
  code_.compare(Reg::x9, Reg::x17, Width::x64);
  code_.branch_far(Condition::no_carry, general);
  code_.alu(Alu::subtract, Reg::x9, Reg::x9, Reg::x17, Width::x64);
  code_.constant(Reg::x17, cycles);
  code_.divide(Reg::x10, Reg::x9, Reg::x17, false, Width::x64);
  code_.compare(Reg::x10, Reg::x11, Width::x64);
  code_.select(Reg::x11, Reg::x11, Reg::x10, Condition::carry, Width::x64);
  code_.branch_zero_far(Reg::x11, general);
}
}
