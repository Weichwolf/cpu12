// SPDX-License-Identifier: GPL-3.0-only
#include "jit/prefixes.hpp"

#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/preparation.hpp"

#include <algorithm>
#include <cstddef>
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
bool transfer_context(const Instruction &instruction) {
  constexpr unsigned selector_mask = 7;
  constexpr unsigned source_shift = 4;
  constexpr unsigned ccr_selector = 2;
  constexpr unsigned stack_selector = 7;
  constexpr unsigned exchange_mask = 0x80;
  const auto destination = instruction.operand & selector_mask;
  const auto source = (instruction.operand >> source_shift) & selector_mask;
  const bool exchange = (instruction.operand & exchange_mask) != 0;
  return destination == ccr_selector || destination == stack_selector ||
         (exchange && (source == ccr_selector || source == stack_selector));
}

bool context_boundary(const Instruction &instruction) {
  return instruction.indexed.indirect ||
         (instruction.indexed.base == IndexBase::sp && instruction.indexed.update != 0) ||
         (instruction.kind == Kind::transfer && transfer_context(instruction)) ||
         ((instruction.kind == Kind::load || instruction.kind == Kind::effective_address ||
           (instruction.kind == Kind::loop && instruction.operation != RegisterOp::test)) &&
          instruction.target == WordTarget::sp) ||
         (instruction.kind == Kind::register_operation &&
          (instruction.operation == RegisterOp::ccr_and ||
           instruction.operation == RegisterOp::ccr_or ||
           instruction.operation == RegisterOp::decimal_adjust));
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

bool owner_boundary(const Instruction &instruction,
                    const Memory &memory,
                    const PrefixAccess access) {
  return context_boundary(instruction) || access == PrefixAccess::boundary ||
         (access == PrefixAccess::ordinary && may_access_owner(instruction, memory));
}

using Entry = std::pair<std::uint64_t, Address>;
using Queue = std::priority_queue<Entry, std::vector<Entry>, std::greater<>>;

void relax(std::map<Address, std::uint64_t> &distances,
           const std::map<Address, std::vector<Edge>> &incoming,
           Queue &pending) {
  constexpr auto infinite = std::numeric_limits<std::uint64_t>::max();
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
}

}

std::map<Address, std::uint64_t>
foreground_prefixes(const std::span<const BlockInstruction> instructions,
                    const Memory &memory,
                    const std::span<const PrefixAccess> access) {
  if (!access.empty() && access.size() != instructions.size()) {
    throw std::invalid_argument("CPU12 prefix access plan must cover every instruction");
  }
  constexpr auto infinite = std::numeric_limits<std::uint64_t>::max();
  std::map<Address, std::uint64_t> distances;
  std::map<Address, std::vector<Edge>> incoming;
  for (const auto &operation : instructions) {
    if (!distances.emplace(operation.pc, infinite).second) {
      throw std::invalid_argument("Duplicate CPU12 foreground prefix instruction");
    }
  }
  Queue pending;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto &operation = instructions[index];
    const auto flow = successors(operation);
    const auto targets = std::span(flow.direct).first(flow.count);
    const auto policy = access.empty() ? PrefixAccess::ordinary : access[index];
    const bool owner = owner_boundary(operation.instruction, memory, policy);
    if (policy == PrefixAccess::terminal && !owner && !flow.indirect) {
      const auto cycles = instruction_timing(operation.instruction).base;
      distances.at(operation.pc) = cycles;
      pending.emplace(cycles, operation.pc);
      continue;
    }
    if (owner || flow.indirect || targets.empty() ||
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
  relax(distances, incoming, pending);
  return distances;
}
}
