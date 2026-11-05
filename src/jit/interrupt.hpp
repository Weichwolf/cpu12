// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"

#include <cstdint>
#include <vector>

namespace cpu12::detail {
void return_interrupt(Code &code, Ram ram);
void defer_interrupt(Code &code, bool accumulator);
void stack_context(Code &code, Ram ram, std::uint8_t next_pc = 0);
void wait_instruction(Code &code, const Instruction &instruction, Ram ram);
[[nodiscard]] std::vector<std::uint8_t>
emit_interrupt(InterruptRequest request, Ram ram, bool reuse);
[[nodiscard]] std::vector<std::uint8_t> emit_stop_wake();
}
