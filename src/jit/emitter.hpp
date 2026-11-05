// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/emission.hpp"

#include <cstdint>
#include <vector>

namespace cpu12::detail {
[[nodiscard]] Emission emit_joined(const Instruction &instruction, const Memory &ram);
[[nodiscard]] std::vector<std::uint8_t> emit(const Instruction &instruction, const Memory &ram);
}
