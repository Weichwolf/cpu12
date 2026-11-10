// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/regions.hpp"

#include "cpu12/memory.hpp"
#include "jit/block.hpp"
#include "jit/preparation.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace cpu12::detail::llvm19 {
namespace {
class Components {
public:
  explicit Components(const std::size_t count) : parent_(count), sizes_(count, 1) {
    for (std::size_t index = 0; index < count; ++index) { parent_[index] = index; }
  }

  std::size_t root(std::size_t node) {
    while (parent_[node] != node) {
      parent_[node] = parent_[parent_[node]];
      node = parent_[node];
    }
    return node;
  }

  void connect(const std::size_t from, const std::size_t to) {
    auto left = root(from);
    auto right = root(to);
    if (left == right) { return; }
    if (sizes_[left] < sizes_[right]) { std::swap(left, right); }
    parent_[right] = left;
    sizes_[left] += sizes_[right];
  }

private:
  std::vector<std::size_t> parent_, sizes_;
};
}

std::vector<std::vector<BlockInstruction>>
regions(const std::span<const BlockInstruction> instructions) {
  std::map<Address, std::size_t> positions;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    positions.emplace(instructions[index].pc, index);
  }
  Components components(instructions.size());
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto flow = successors(instructions[index]);
    for (const auto address : std::span(flow.direct).first(flow.count)) {
      const auto found = positions.find(address);
      if (found != positions.end()) { components.connect(index, found->second); }
    }
  }
  std::vector<std::vector<BlockInstruction>> result;
  std::map<std::size_t, std::size_t> groups;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto [group, inserted] = groups.try_emplace(components.root(index), result.size());
    if (inserted) { result.emplace_back(); }
    result[group->second].push_back(instructions[index]);
  }
  for (auto &region : result) { std::ranges::sort(region, {}, &BlockInstruction::pc); }
  return result;
}
}
