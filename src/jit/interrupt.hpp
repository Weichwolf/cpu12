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
void return_interrupt(Code &code, const Memory &ram);
void defer_interrupt(Code &code, bool accumulator);
void stack_context(Code &code, const Memory &ram, std::uint8_t next_pc = 0);
void wait_instruction(Code &code, const Instruction &instruction, const Memory &ram);
[[nodiscard]] std::vector<std::uint8_t> emit_interrupt(InterruptRequest request,
                                                       const Memory &ram,
                                                       InterruptFrame frame,
                                                       std::uint8_t next_pc = 0);
[[nodiscard]] std::vector<std::uint8_t> emit_stop_wake();
}
