// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "cpu12/memory.hpp"
#include "instruction.hpp"

#include <cstdint>

namespace cpu12::detail {
// S12CPUV2 nominal internal-bus periods. Board waits and oscillator routing stay
// with the consumer. Conditional deltas belong to the path actually executed.
struct InstructionTiming {
  unsigned base = 0;
  unsigned taken_extra = 0;
};

inline constexpr unsigned maximum_instruction_cycles = 12;
[[nodiscard]] InstructionTiming instruction_timing(const Instruction &instruction);
[[nodiscard]] bool may_access_owner(const Instruction &instruction, const Memory &memory);
[[nodiscard]] bool branch_taken(const Instruction &instruction, std::uint8_t ccr);
}
