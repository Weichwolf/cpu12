// SPDX-License-Identifier: GPL-3.0-only
#include "jit/prefixes.hpp"

#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/preparation.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpu12::detail {
namespace {
bool context_boundary(const Instruction &instruction) {
  // New stack or mask state invalidates an interrupt-entry guarantee.
  return instruction.indexed.indirect ||
         (instruction.indexed.base == IndexBase::sp && instruction.indexed.update != 0) ||
         instruction.kind == Kind::transfer ||
         ((instruction.kind == Kind::load || instruction.kind == Kind::effective_address ||
           (instruction.kind == Kind::loop && instruction.operation != RegisterOp::test)) &&
          instruction.target == WordTarget::sp) ||
         (instruction.kind == Kind::register_operation &&
          (instruction.operation == RegisterOp::ccr_and ||
           instruction.operation == RegisterOp::ccr_or));
}

unsigned edge_cycles(const Instruction &instruction, const unsigned edge) {
  const auto timing = instruction_timing(instruction);
  const bool taken = (instruction.kind == Kind::branch && instruction.condition == 0) ||
                     ((instruction.kind == Kind::branch || instruction.kind == Kind::bit_branch ||
                       instruction.kind == Kind::loop) &&
                      edge != 0);
  return timing.base + (taken ? timing.taken_extra : 0);
}

struct Edge {
  Address from;
  unsigned cycles;
};

using Entry = std::pair<std::uint64_t, Address>;
using Queue = std::priority_queue<Entry, std::vector<Entry>, std::greater<>>;
}

std::map<Address, std::uint64_t>
foreground_prefixes(const std::span<const BlockInstruction> instructions, const Memory &memory) {
  constexpr auto infinite = std::numeric_limits<std::uint64_t>::max();
  std::map<Address, std::uint64_t> distances;
  std::map<Address, std::vector<Edge>> incoming;
  for (const auto &operation : instructions) {
    if (!distances.emplace(operation.pc, infinite).second) {
      throw std::invalid_argument("Duplicate CPU12 foreground prefix instruction");
    }
  }
  Queue pending;
  for (const auto &operation : instructions) {
    const auto flow = successors(operation);
    const auto targets = std::span(flow.direct).first(flow.count);
    if (context_boundary(operation.instruction) ||
        may_access_owner(operation.instruction, memory) || flow.indirect || targets.empty() ||
        std::ranges::any_of(targets,
                            [&](const auto target) { return !distances.contains(target); })) {
      distances.at(operation.pc) = 0;
      pending.emplace(0, operation.pc);
      continue;
    }
    for (unsigned edge = 0; edge < flow.count; ++edge) {
      incoming[flow.direct[edge]].push_back(
          {.from = operation.pc, .cycles = edge_cycles(operation.instruction, edge)});
    }
  }
  while (!pending.empty()) {
    const auto [cycles, pc] = pending.top();
    pending.pop();
    if (cycles != distances.at(pc)) { continue; }
    const auto found = incoming.find(pc);
    if (found == incoming.end()) { continue; }
    for (const auto edge : found->second) {
      if (cycles > infinite - edge.cycles) { continue; }
      const auto candidate = cycles + edge.cycles;
      auto &before = distances.at(edge.from);
      if (candidate < before) {
        before = candidate;
        pending.emplace(candidate, edge.from);
      }
    }
  }
  return distances;
}
}
