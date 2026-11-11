// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "jit/block.hpp"

#include <cstdint>
#include <map>
#include <span>

namespace cpu12::detail {
[[nodiscard]] std::map<Address, std::uint64_t>
foreground_prefixes(std::span<const BlockInstruction> instructions, const Memory &memory);
}
