// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "jit/block.hpp"

#include <cstddef>
#include <map>
#include <set>
#include <span>

namespace cpu12::detail::llvm19 {
struct GrantCost {
  unsigned instructions = 0, cycles = 0;
};

struct LoopGrant {
  std::size_t first, last;
  GrantCost cost;
};

// Forward paths are acyclic; backwards edges and native transfers start grants.
// A straight natural loop admits a quota of whole turns, including early exits.
class Grants {
public:
  explicit Grants(std::span<const BlockInstruction> instructions);

  [[nodiscard]] bool head(Address pc) const { return heads_.contains(pc); }

  [[nodiscard]] bool single(Address pc) const { return single_.contains(pc); }

  [[nodiscard]] GrantCost ahead(Address pc) const { return ahead_.at(pc); }

  [[nodiscard]] const std::map<Address, LoopGrant> &loops() const { return loops_; }

private:
  void collect_heads(std::span<const BlockInstruction> instructions);
  void summarize(std::span<const BlockInstruction> instructions);
  void capture(std::span<const BlockInstruction> instructions, std::size_t first, std::size_t last);
  std::set<Address> heads_, forward_, single_;
  std::map<Address, GrantCost> ahead_;
  std::map<Address, LoopGrant> loops_;
};
}
