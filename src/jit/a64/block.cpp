// SPDX-License-Identifier: GPL-3.0-only
#include "jit/block.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"
#include "jit/resident.hpp"
#include "runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::detail::a64 {
namespace {
Address following(const BlockInstruction &operation) {
  return static_cast<Address>(operation.pc + operation.instruction.length);
}

Address target(const BlockInstruction &operation) {
  const auto &instruction = operation.instruction;
  if (instruction.kind == Kind::branch || instruction.kind == Kind::loop) {
    return static_cast<Address>(following(operation) + instruction.operand);
  }
  if (instruction.kind == Kind::jump && instruction.addressing == Addressing::extended) {
    return instruction.address;
  }
  return following(operation);
}

std::vector<BlockInstruction> loop_tail(const std::span<const BlockInstruction> operations,
                                        const Memory &memory) {
  const auto &last = operations.back();
  if (last.instruction.kind != Kind::branch || last.instruction.condition < 2) { return {}; }
  auto tail = parse_block(memory, following(last));
  if (tail.empty() || target(last) != following(tail.back()) ||
      target(tail.back()) != operations.front().pc) {
    return {};
  }
  const auto &backedge = tail.back().instruction;
  if ((backedge.kind == Kind::branch && backedge.condition == 0) ||
      (backedge.kind == Kind::jump && backedge.addressing == Addressing::extended)) {
    return tail;
  }
  return {};
}
}

void Emitter::clock(const unsigned cycles) {
  if (cycles == 0) { return; }
  constexpr auto offset = offsetof(NativeContext, progress) + offsetof(Progress, estimated_cycles);
  code_.load(Reg::x9, context, offset, Access::doubleword);
  code_.constant(Reg::x10, cycles);
  code_.alu(Alu::add, Reg::x9, Reg::x9, Reg::x10, Width::x64);
  code_.store(Reg::x9, context, offset, Access::doubleword);
}

void Emitter::commit(const unsigned instructions, const Progress elapsed) {
  if (instructions == 0) { return; }
  clock(static_cast<unsigned>(elapsed.estimated_cycles));
  code_.constant(Reg::x17, instructions);
  code_.alu(Alu::add, retired, retired, Reg::x17);
  code_.alu(Alu::subtract, remaining, remaining, Reg::x17);
}

void Emitter::control_clock(const Instruction &instruction) {
  const auto extra = instruction_timing(instruction).taken_extra;
  if (extra == 0) { return; }
  Label skip;
  if (instruction.kind == Kind::stop) {
    code_.load(
        Reg::x9, context, offsetof(NativeContext, cpu) + offsetof(State, execution), Access::byte);
    code_.constant(Reg::x10, static_cast<unsigned>(ExecutionState::stopped));
    code_.compare(Reg::x9, Reg::x10);
    code_.branch_far(Condition::not_equal, skip);
  } else if (instruction.kind != Kind::branch || instruction.condition == 1) {
    return;
  } else if (instruction.condition != 0) {
    branch_guard(instruction, skip);
  }
  clock(extra);
  code_.bind(skip);
}

void Emitter::yield_boundary() {
  code_.load(Reg::x9, context, offsetof(NativeContext, yield_requested), Access::byte);
  Label proceed;
  code_.branch_zero_far(Reg::x9, proceed);
  constexpr unsigned location_offset = 24;
  code_.load(Reg::x9, Reg::sp, location_offset);
  code_.constant(Reg::x10, native_boundary_mask);
  code_.alu(Alu::bit_or, Reg::x9, Reg::x9, Reg::x10);
  code_.store(Reg::x9, Reg::sp, location_offset);
  code_.branch(done_);
  code_.bind(proceed);
}

void Emitter::sequence(const std::span<const BlockInstruction> operations) {
  unsigned required = 0;
  for (const auto &item : operations) { required += instruction_timing(item.instruction).base; }
  required += instruction_timing(operations.back().instruction).taken_extra;
  code_.load(Reg::x9, context, offsetof(NativeContext, cycle_deadline), Access::doubleword);
  constexpr auto offset = offsetof(NativeContext, progress) + offsetof(Progress, estimated_cycles);
  code_.load(Reg::x10, context, offset, Access::doubleword);
  code_.compare(Reg::x9, Reg::x10, Width::x64);
  code_.branch_far(Condition::no_carry, done_);
  code_.alu(Alu::subtract, Reg::x9, Reg::x9, Reg::x10, Width::x64);
  code_.constant(Reg::x17, required);
  code_.compare(Reg::x9, Reg::x17, Width::x64);
  code_.branch_far(Condition::no_carry, done_);
  code_.constant(Reg::x17, operations.size());
  code_.compare(remaining, Reg::x17);
  code_.branch_far(Condition::no_carry, done_);
  unsigned count = 0;
  unsigned cycles = 0;
  for (const auto &item : operations) {
    const bool owner = may_access_owner(item.instruction, memory_);
    const bool fault_boundary = owner || (item.instruction.kind == Kind::register_operation &&
                                          item.instruction.operation == RegisterOp::decimal_adjust);
    if (fault_boundary) {
      commit(count, {.estimated_cycles = cycles});
      count = 0;
      cycles = 0;
      location(item.instruction);
    }
    operation(item.instruction);
    ++count;
    cycles += instruction_timing(item.instruction).base;
    if (owner) {
      commit(count, {.estimated_cycles = cycles});
      control_clock(item.instruction);
      count = 0;
      cycles = 0;
      yield_boundary();
    }
  }
  commit(count, {.estimated_cycles = cycles});
  if (count != 0) { control_clock(operations.back().instruction); }
  const auto &last = operations.back();
  const bool boundary = last.instruction.kind == Kind::wait || last.instruction.kind == Kind::stop;
  code_.constant(Reg::x17,
                 std::uint32_t{last.pc} |
                     (std::uint32_t{last.instruction.opcode} << native_opcode_shift) |
                     (boundary ? native_boundary_mask : 0U));
  constexpr unsigned location_offset = 24;
  code_.store(Reg::x17, Reg::sp, location_offset);
}

std::vector<Byte> Emitter::block(const std::span<const BlockInstruction> operations) {
  if (operations.empty()) { throw std::invalid_argument("Cannot emit an empty A64 block"); }
  const auto tail = loop_tail(operations, memory_);
  const bool cyclic = !tail.empty() || target(operations.back()) == operations.front().pc;
  begin(true);
  const auto &last = operations.back();
  const bool boundary = last.instruction.kind == Kind::wait || last.instruction.kind == Kind::stop;
  code_.constant(Reg::x17,
                 std::uint32_t{last.pc} |
                     (std::uint32_t{last.instruction.opcode} << native_opcode_shift) |
                     (boundary ? native_boundary_mask : 0U));
  constexpr unsigned location_offset = 24;
  code_.store(Reg::x17, Reg::sp, location_offset);
  Label repeat;
  code_.bind(repeat);
  sequence(operations);
  if (cyclic && !boundary) {
    code_.constant(Reg::x17, operations.front().pc);
    code_.compare(pc, Reg::x17);
    code_.branch_far(Condition::equal, repeat);
    if (!tail.empty()) {
      code_.constant(Reg::x17, tail.front().pc);
      code_.compare(pc, Reg::x17);
      code_.branch_far(Condition::not_equal, done_);
      sequence(tail);
      code_.branch(repeat);
    }
  }
  auto general = finish();
  if (const auto region = resident_region_plan(operations, memory_)) {
    return Emitter(memory_).resident_region(*region, general);
  }
  auto prefix = operations;
  const auto kind = prefix.back().instruction.kind;
  if (prefix.size() > 1 && (kind == Kind::stop || kind == Kind::wait || kind == Kind::jump)) {
    prefix = prefix.first(prefix.size() - 1);
  }
  auto plan = resident_loop_plan(prefix, memory_);
  if (!plan) { plan = resident_plan(prefix, memory_); }
  if (plan && (tail.empty() || plan->loop)) {
    return Emitter(memory_).resident(*plan, operations.subspan(prefix.size()), general);
  }
  return general;
}
}
