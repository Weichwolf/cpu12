// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"

#include <cstdint>
#include <vector>

namespace cpu12::detail {
enum class InterruptFrame : std::uint8_t { push, waiting, returned };
inline constexpr auto stop_event_key = std::uint64_t{3} << 62U;

[[nodiscard]] constexpr std::uint64_t event_key(const InterruptRequest request,
                                                const InterruptFrame frame) {
  constexpr unsigned kind_shift = 16;
  constexpr unsigned frame_shift = 17;
  constexpr unsigned event_shift = 63;
  return (std::uint64_t{1} << event_shift) | (static_cast<std::uint64_t>(frame) << frame_shift) |
         (static_cast<std::uint64_t>(request.kind) << kind_shift) | request.vector;
}

void return_interrupt(Code &code, const Memory &ram);
void defer_interrupt(Code &code, bool accumulator);
void stack_context(Code &code, const Memory &ram, std::uint8_t next_pc = 0);
void wait_instruction(Code &code, const Instruction &instruction, const Memory &ram);

// At a connected exit, D/BX, Y/DX, SP/BP and CCR/CL may feed a proven RAM frame.
// The original bus path still reads published CPU state across real callbacks.
struct WaitValues {
  Address pc;
  unsigned words;
};

[[nodiscard]] Emission
emit_resident_wait(const Instruction &instruction, const Memory &ram, WaitValues values);
[[nodiscard]] std::vector<std::uint8_t> emit_interrupt(InterruptRequest request,
                                                       const Memory &ram,
                                                       InterruptFrame frame,
                                                       std::uint8_t next_pc = 0);
[[nodiscard]] std::vector<std::uint8_t> emit_stop_wake();
}
