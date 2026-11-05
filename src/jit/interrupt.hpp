// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/code.hpp"

#include <cstdint>
#include <vector>

namespace cpu12::detail {
void return_interrupt(Code &code, Ram ram);
void defer_interrupt(Code &code, bool accumulator);
[[nodiscard]] std::vector<std::uint8_t> emit_interrupt(InterruptRequest request, Ram ram);
}
