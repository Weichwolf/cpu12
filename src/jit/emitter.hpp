// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"

#include <cstdint>
#include <vector>

namespace cpu12::detail {
[[nodiscard]] std::vector<std::uint8_t> emit(const Instruction &instruction);
}
