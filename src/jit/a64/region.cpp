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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::detail::a64 {
namespace {
constexpr unsigned location_offset = 24;
constexpr auto progress_offset =
    offsetof(NativeContext, progress) + offsetof(Progress, estimated_cycles);

unsigned cycles(const ResidentPlan &plan) {
  unsigned result = 0;
  for (const auto &operation : plan.operations) {
    result += instruction_timing(operation.instruction).base;
  }
  return result;
}

std::optional<ResidentPlan> counted_region(const ResidentRegion &region) {
  const auto &head = region.head.operations;
  const auto &tail = region.tail.operations;
  const auto &update = tail[tail.size() - 2].instruction;
  constexpr unsigned not_equal = 6;
  if (tail.back().instruction.condition != not_equal || update.kind != Kind::register_operation ||
      (update.operation != RegisterOp::index_increment &&
       update.operation != RegisterOp::index_decrement)) {
    return std::nullopt;
  }
  const auto &guard = head[head.size() - 2].instruction;
  if (guard.kind != Kind::compare || region.head.changed != written_flags(guard) ||
      instruction_timing(head.back().instruction).taken_extra > cycles(region.tail)) {
    return std::nullopt;
  }
  auto combined = region.head;
  combined.operations.insert(combined.operations.end(), tail.begin(), tail.end());
  const auto counter = 1U << native_word(update);
  const auto writes = std::ranges::count_if(combined.operations, [counter](const auto &operation) {
    return (written_words(operation.instruction) & counter) != 0;
  });
  if (writes != 1) { return std::nullopt; }
  combined.loop = true;
  combined.increment = update.operation == RegisterOp::index_increment;
  combined.counter_offset =
      update.target == WordTarget::x ? offsetof(Registers, x) : offsetof(Registers, y);
  return combined;
}
}

void Emitter::resident_region_exit(const ResidentRegion &region, const bool head) {
  const auto count = region.head.operations.size() + region.tail.operations.size();
  const auto &last = head ? region.head.operations.back() : region.tail.operations.back();
  const auto extra = instruction_timing(last.instruction).taken_extra;
  const auto full_cycles =
      cycles(region.head) + cycles(region.tail) +
      instruction_timing(region.tail.operations.back().instruction).taken_extra;
  code_.constant(Reg::x17, count);
  code_.multiply(retired, Reg::x11, Reg::x17);
  code_.constant(Reg::x17, full_cycles);
  code_.multiply(Reg::x13, Reg::x11, Reg::x17, Width::x64);
  if (head) {
    code_.constant(Reg::x17, region.head.operations.size());
    code_.alu(Alu::add, retired, retired, Reg::x17);
    code_.constant(Reg::x17, cycles(region.head) + extra);
    code_.alu(Alu::add, Reg::x13, Reg::x13, Reg::x17, Width::x64);
  } else {
    code_.subtract(Reg::x13, Reg::x13, static_cast<std::uint16_t>(extra), Width::x64);
  }
  code_.alu(Alu::add, Reg::x14, Reg::x14, Reg::x13, Width::x64);
  code_.store(Reg::x14, context, progress_offset, Access::doubleword);
  const auto &tail = region.tail.operations.back();
  code_.constant(pc, static_cast<Address>(tail.pc + tail.instruction.length));
  code_.constant(Reg::x17,
                 std::uint32_t{last.pc} |
                     (std::uint32_t{last.instruction.opcode} << native_opcode_shift));
  code_.store(Reg::x17, Reg::sp, location_offset);
}

std::vector<Byte> Emitter::resident_region(const ResidentRegion &region,
                                           const std::span<const Byte> general) {
  auto combined = counted_region(region);
  if (!combined) { return {general.begin(), general.end()}; }
  Label fallback;
  resident_grant(*combined, {}, fallback);
  begin(true);
  combined->loop = false;
  resident_prepare(*combined);
  code_.constant(Reg::x11, 0);
  auto prefix = region.head;
  prefix.operations.resize(prefix.operations.size() - 2);
  const auto &guard = region.head.operations[region.head.operations.size() - 2];
  const auto &branch = region.head.operations.back().instruction;
  const auto &update = region.tail.operations[region.tail.operations.size() - 2].instruction;
  const auto counter = word_target(update.target);
  const auto final_counter = combined->increment ? word_mask : 1U;
  Label repeat;
  Label head_exit;
  Label final_tail;
  Label complete;
  code_.bind(repeat);
  resident_body(prefix, false);
  resident_calculate(guard);
  code_.compare(Reg::x0, Reg::x1);
  switch (branch.condition) {
    case 4: code_.branch_far(Condition::carry, head_exit); break;
    case 5: code_.branch_far(Condition::no_carry, head_exit); break;
    case 6: code_.branch_far(Condition::not_equal, head_exit); break;
    case 7: code_.branch_far(Condition::equal, head_exit); break;
    default: throw std::logic_error("Unsupported ARM64 resident region guard");
  }
  code_.constant(Reg::x17, final_counter);
  code_.compare(counter, Reg::x17);
  code_.branch_far(Condition::equal, final_tail);
  resident_body(region.tail, false);
  code_.add(Reg::x11, Reg::x11, 1);
  code_.branch(repeat);
  code_.bind(final_tail);
  resident_body(region.tail, true);
  code_.add(Reg::x11, Reg::x11, 1);
  resident_region_exit(region, false);
  code_.branch(complete);
  code_.bind(head_exit);
  resident_flags(guard);
  resident_region_exit(region, true);
  code_.bind(complete);
  if (swapped_d_) { code_.reverse_half(d, d); }
  static_cast<void>(finish());
  code_.bind(fallback);
  code_.append(general);
  return code_.bytes();
}
}
