// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/block.hpp"

#include <array>
#include <span>
#include <vector>

namespace cpu12::detail {
struct Successors {
  std::array<Address, 2> direct{};
  unsigned count = 0;
  bool indirect = false;
};

struct PreparationGraph {
  std::vector<BlockInstruction> instructions;
  std::vector<Address> unresolved;
  RunResult failure;
};

[[nodiscard]] Successors successors(const BlockInstruction &operation);
[[nodiscard]] PreparationGraph preparation_graph(const Memory &memory,
                                                 std::span<const Address> established_entries);
}
