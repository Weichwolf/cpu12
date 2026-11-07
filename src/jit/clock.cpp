// SPDX-License-Identifier: GPL-3.0-only
#include "jit/clock.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace cpu12::detail {
namespace {
constexpr auto cycle_offset =
    offsetof(NativeContext, progress) + offsetof(Progress, estimated_cycles);

void remaining_cycles(Code &code, const ClockContext context) {
  const bool block = context == ClockContext::block;
  const std::array<std::uint8_t, 3> load{static_cast<std::uint8_t>(block ? 0x49 : 0x48),
                                         0x8b,
                                         static_cast<std::uint8_t>(block ? 0x85 : 0x86)};
  code.raw(load);
  code.u32(offsetof(NativeContext, cycle_deadline));
  const std::array<std::uint8_t, 3> subtract{static_cast<std::uint8_t>(block ? 0x49 : 0x48),
                                             0x2b,
                                             static_cast<std::uint8_t>(block ? 0x85 : 0x86)};
  code.raw(subtract);
  code.u32(cycle_offset);
  constexpr std::array<std::uint8_t, 2> nonnegative{0x0f, 0x83};
  code.raw(nonnegative);
  const auto positive = code.relative32();
  constexpr std::array<std::uint8_t, 2> zero{0x31, 0xc0};
  code.raw(zero);
  code.bind(positive);
}

void add_constant(Code &code,
                  const std::size_t offset,
                  const ClockContext context,
                  const std::uint64_t value) {
  if (value == 0) { return; }
  if (value > std::numeric_limits<std::int32_t>::max()) {
    throw std::length_error("CPU12 static timing group exceeds native immediate");
  }
  const bool block = context == ClockContext::block;
  const std::array<std::uint8_t, 3> add{static_cast<std::uint8_t>(block ? 0x49 : 0x48),
                                        0x81,
                                        static_cast<std::uint8_t>(block ? 0x85 : 0x86)};
  code.raw(add);
  code.u32(static_cast<std::uint32_t>(offset));
  code.u32(static_cast<std::uint32_t>(value));
}

void add_register(Code &code, const ClockContext context, const std::size_t offset) {
  const bool block = context == ClockContext::block;
  const std::array<std::uint8_t, 3> add{static_cast<std::uint8_t>(block ? 0x49 : 0x48),
                                        0x01,
                                        static_cast<std::uint8_t>(block ? 0x85 : 0x86)};
  code.raw(add);
  code.u32(static_cast<std::uint32_t>(offset));
}

void adjust_accumulator(Code &code, const std::int64_t amount) {
  if (amount == 0) { return; }
  if (amount < std::numeric_limits<std::int32_t>::min() ||
      amount > std::numeric_limits<std::int32_t>::max()) {
    throw std::length_error("CPU12 static timing correction exceeds native immediate");
  }
  constexpr std::array<std::uint8_t, 2> add{0x48, 0x05};
  code.raw(add);
  code.u32(static_cast<std::uint32_t>(amount));
}

std::size_t branch_not_taken(Code &code,
                             const Instruction &instruction,
                             const Address next,
                             const ClockContext context) {
  const bool block = context == ClockContext::block;
  if (instruction.operand != 0) {
    constexpr std::array<std::uint8_t, 3> argument_pc{0x66, 0x81, 0x3f};
    constexpr std::array<std::uint8_t, 5> block_pc{0x66, 0x41, 0x81, 0x3c, 0x24};
    if (block) {
      code.raw(block_pc);
    } else {
      code.raw(argument_pc);
    }
    code.u16(static_cast<Address>(next + instruction.operand));
    constexpr std::array<std::uint8_t, 2> not_equal{0x0f, 0x85};
    code.raw(not_equal);
    return code.relative32();
  }
  constexpr auto ccr = static_cast<std::uint8_t>(offsetof(Registers, ccr));
  constexpr std::array<std::uint8_t, 4> argument_ccr{0x0f, 0xb6, 0x47, ccr};
  constexpr std::array<std::uint8_t, 6> block_ccr{0x41, 0x0f, 0xb6, 0x44, 0x24, ccr};
  if (block) {
    code.raw(block_ccr);
  } else {
    code.raw(argument_ccr);
  }
  constexpr unsigned signed_condition = 12;
  if (instruction.condition >= signed_condition) {
    constexpr std::array<std::uint8_t, 10> sign{
        0x83, 0xe0, 0x0e, 0x89, 0xc1, 0xc1, 0xe9, 0x02, 0x31, 0xc8};
    code.raw(sign);
  }
  constexpr std::array<std::uint8_t, 16> masks{0, 0, 5, 5, 1, 1, 4, 4, 2, 2, 8, 8, 2, 2, 6, 6};
  constexpr std::uint8_t test = 0xa8;
  code.byte(test);
  code.byte(masks.at(instruction.condition));
  code.byte(0x0f);
  constexpr std::uint8_t equal = 0x84;
  constexpr std::uint8_t not_equal = 0x85;
  code.byte((instruction.condition & 1U) != 0 ? equal : not_equal);
  return code.relative32();
}
}

std::size_t emit_horizon_guard(Code &code, const Progress required, const ClockContext context) {
  if (required.estimated_cycles > std::numeric_limits<std::int32_t>::max()) {
    throw std::length_error("CPU12 static horizon exceeds native immediate");
  }
  remaining_cycles(code, context);
  constexpr std::array<std::uint8_t, 2> compare{0x48, 0x3d};
  code.raw(compare);
  code.u32(static_cast<std::uint32_t>(required.estimated_cycles));
  constexpr std::array<std::uint8_t, 2> below{0x0f, 0x82};
  code.raw(below);
  return code.relative32();
}

void emit_cycle_grant(Code &code, const ClockScale scale) {
  if (scale.cycles_per_iteration == 0) { throw std::logic_error("Empty CPU12 cycle grant"); }
  constexpr std::array<std::uint8_t, 3> limit{0x48, 0x8b, 0x86};
  code.raw(limit);
  code.u32(offsetof(NativeContext, cycle_deadline));
  constexpr std::array<std::uint8_t, 6> unlimited{0x48, 0x83, 0xf8, 0xff, 0x0f, 0x84};
  code.raw(unlimited);
  const auto ordinary = code.relative32();
  constexpr std::array<std::uint8_t, 3> budget{0x41, 0x89, 0xd0}; // MOV R8D,EDX.
  code.raw(budget);
  remaining_cycles(code, ClockContext::argument);
  if (scale.residual_cycles != 0) {
    constexpr std::array<std::uint8_t, 2> subtract{0x48, 0x2d};
    code.raw(subtract);
    code.u32(scale.residual_cycles);
    constexpr std::array<std::uint8_t, 2> nonnegative{0x0f, 0x83};
    code.raw(nonnegative);
    const auto enough = code.relative32();
    constexpr std::array<std::uint8_t, 2> zero{0x31, 0xc0};
    code.raw(zero);
    code.bind(enough);
  }
  constexpr std::array<std::uint8_t, 3> divisor{0x31, 0xd2, 0xb9};
  code.raw(divisor);
  code.u32(scale.cycles_per_iteration);
  constexpr std::array<std::uint8_t, 3> divide{0x48, 0xf7, 0xf1};
  code.raw(divide);
  constexpr std::array<std::uint8_t, 3> multiply{0x48, 0x69, 0xc0};
  code.raw(multiply);
  code.u32(scale.instructions_per_iteration);
  adjust_accumulator(code, scale.residual_instructions);
  constexpr std::array<std::uint8_t, 8> clamp{0x4c, 0x39, 0xc0, 0x49, 0x0f, 0x47, 0xc0, 0x89};
  code.raw(clamp); // Clamp RAX to original R8 budget, then MOV EDX,EAX.
  constexpr std::uint8_t to_edx = 0xc2;
  code.byte(to_edx);
  code.bind(ordinary);
}

std::size_t emit_yield_check(Code &code, const ClockContext context) {
  const bool block = context == ClockContext::block;
  const std::array<std::uint8_t, 3> compare{static_cast<std::uint8_t>(block ? 0x41 : 0x40),
                                            0x80,
                                            static_cast<std::uint8_t>(block ? 0xbd : 0xbe)};
  code.raw(compare);
  code.u32(offsetof(NativeContext, yield_requested));
  code.byte(0);
  constexpr std::array<std::uint8_t, 2> requested{0x0f, 0x85};
  code.raw(requested);
  return code.relative32();
}

void emit_progress(Code &code, const Progress delta, const ClockContext context) {
  add_constant(code, cycle_offset, context, delta.estimated_cycles);
}

void emit_loop_progress(Code &code, const ClockScale scale, const ClockContext context) {
  if (scale.instructions_per_iteration == 0) { throw std::logic_error("Empty CPU12 timing loop"); }
  constexpr std::array<std::uint8_t, 3> original{0x49, 0x89, 0xc0}; // MOV R8,RAX.
  code.raw(original);
  adjust_accumulator(code, std::int64_t{scale.instruction_bias} - scale.residual_instructions);
  if (scale.instructions_per_iteration != 1) {
    constexpr std::array<std::uint8_t, 3> divisor{0x31, 0xd2, 0xb9};
    code.raw(divisor); // XOR EDX,EDX; MOV ECX,complete original body size.
    code.u32(scale.instructions_per_iteration);
    constexpr std::array<std::uint8_t, 3> divide{0x48, 0xf7, 0xf1}; // DIV RCX.
    code.raw(divide);
  }
  constexpr std::array<std::uint8_t, 3> multiply{0x48, 0x69, 0xc0};
  code.raw(multiply);
  code.u32(scale.cycles_per_iteration);
  adjust_accumulator(code, std::int64_t{scale.cycle_bias} + scale.residual_cycles);
  add_register(code, context, cycle_offset);
  constexpr std::array<std::uint8_t, 3> restore{0x4c, 0x89, 0xc0}; // MOV RAX,R8.
  code.raw(restore);
}

void emit_control_cycles(Code &code,
                         const Instruction &instruction,
                         const Address next,
                         const ClockContext context) {
  const auto extra = instruction_timing(instruction).taken_extra;
  if (extra == 0) { return; }
  if (instruction.kind == Kind::stop) {
    const bool block = context == ClockContext::block;
    const std::array<std::uint8_t, 3> compare{static_cast<std::uint8_t>(block ? 0x41 : 0x40),
                                              0x80,
                                              static_cast<std::uint8_t>(block ? 0xbd : 0xbe)};
    code.raw(compare);
    code.u32(offsetof(NativeContext, cpu) + offsetof(State, execution));
    code.byte(static_cast<std::uint8_t>(ExecutionState::stopped));
    constexpr std::array<std::uint8_t, 2> different{0x0f, 0x85};
    code.raw(different);
    const auto skip = code.relative32();
    emit_progress(code, {.estimated_cycles = extra}, context);
    code.bind(skip);
    return;
  }
  if (instruction.kind != Kind::branch || instruction.condition == 1) { return; }
  if (instruction.condition == 0) {
    emit_progress(code, {.estimated_cycles = extra}, context);
    return;
  }
  constexpr std::uint8_t push = 0x50;
  constexpr std::uint8_t pop = 0x58;
  const bool preserve = instruction.operand == 0;
  if (preserve) { code.byte(push); }
  const auto skip = branch_not_taken(code, instruction, next, context);
  emit_progress(code, {.estimated_cycles = extra}, context);
  code.bind(skip);
  if (preserve) { code.byte(pop); }
}
}
