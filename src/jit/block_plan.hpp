// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/emission.hpp"
#include "jit/resident.hpp"

#include <optional>
#include <span>
#include <vector>

namespace cpu12::detail {
[[nodiscard]] std::vector<BlockInstruction> resident_terminal(const Memory &memory,
                                                              Address address);
[[nodiscard]] std::optional<unsigned> flag_writes(const Instruction &instruction,
                                                  const Memory &memory);
[[nodiscard]] std::vector<EmissionOptions>
emission_options(std::span<const BlockInstruction> instructions, const Memory &memory);
[[nodiscard]] std::optional<ResidentPlan>
resident_prefix_plan(std::span<const BlockInstruction> instructions, const Memory &memory);
}
