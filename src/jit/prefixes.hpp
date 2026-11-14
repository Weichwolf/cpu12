// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/block.hpp"

#include <cstdint>
#include <map>
#include <span>

namespace cpu12::detail {
[[nodiscard]] bool private_frame(const Memory &memory, Address stack);

[[nodiscard]] std::map<Address, std::uint64_t>
foreground_prefixes(std::span<const BlockInstruction> instructions, const Memory &memory);

[[nodiscard]] std::uint64_t
interrupt_entry_prefix(const State &state,
                       const Memory &memory,
                       const std::map<Address, std::uint64_t> &foreground,
                       std::span<const InterruptRequest> interrupts);
}
