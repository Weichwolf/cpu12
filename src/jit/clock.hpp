// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "cpu12/core.hpp"
#include "instruction.hpp"

#include <cstddef>
#include <cstdint>

namespace cpu12::detail {
class Code;
enum class ClockContext : std::uint8_t { argument, block };

// Compile-time accounting for an already admitted native loop. RAX carries the
// actual retired count; no accounting instructions are inserted into its body.
struct ClockScale {
  unsigned instructions_per_iteration = 1;
  unsigned cycles_per_iteration = 0;
  unsigned residual_instructions = 0;
  unsigned residual_cycles = 0;
  int instruction_bias = 0;
  int cycle_bias = 0;
};

[[nodiscard]] std::size_t emit_yield_check(Code &code, ClockContext context);
[[nodiscard]] std::size_t emit_horizon_guard(Code &code, Progress required, ClockContext context);
void emit_cycle_grant(Code &code, ClockScale scale);
void emit_progress(Code &code, Progress delta, ClockContext context);
void emit_loop_progress(Code &code, ClockScale scale, ClockContext context);
void emit_control_cycles(Code &code,
                         const Instruction &instruction,
                         Address next,
                         ClockContext context);
}
