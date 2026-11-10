// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "jit/block.hpp"

#include <span>
#include <vector>

namespace cpu12::detail::llvm19 {
// Keep every established direct edge inside a region. Unsupported boundaries
// separate unrelated native functions; neither budgets nor length split code.
[[nodiscard]] std::vector<std::vector<BlockInstruction>>
regions(std::span<const BlockInstruction> instructions);
}
