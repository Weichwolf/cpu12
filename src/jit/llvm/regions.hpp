// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "jit/block.hpp"

#include <span>
#include <vector>

namespace cpu12::detail::llvm19 {
// Keep call entries distinct; established cross-region edges become native
// transfers. Neither budgets nor instruction counts split code.
[[nodiscard]] std::vector<std::vector<BlockInstruction>>
regions(std::span<const BlockInstruction> instructions);
}
