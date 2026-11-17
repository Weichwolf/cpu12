// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/grants.hpp"

#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/preparation.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <span>

namespace cpu12::detail::llvm19 {
Grants::Grants(const std::span<const BlockInstruction> instructions) {
  collect_heads(instructions);
  std::map<Address, std::size_t> indices;
  for (std::size_t i = 0; i < instructions.size(); ++i) { indices.emplace(instructions[i].pc, i); }
  for (std::size_t last = 0; last < instructions.size(); ++last) {
    const auto &operation = instructions[last];
    const auto &instruction = operation.instruction;
    if (instruction.kind != Kind::branch && instruction.kind != Kind::loop &&
        instruction.kind != Kind::bit_branch) {
      continue;
    }
    const auto target =
        static_cast<Address>(operation.pc + instruction.length + instruction.operand);
    const auto first = indices.find(target);
    if (first == indices.end() || first->second > last ||
        (instruction.kind == Kind::branch && instruction.condition == 1)) {
      continue;
    }
    capture(instructions, first->second, last);
  }
  summarize(instructions);
}

void Grants::collect_heads(const std::span<const BlockInstruction> instructions) {
  for (std::size_t i = 0; i < instructions.size(); ++i) {
    const auto &operation = instructions[i];
    const auto flow = successors(operation);
    for (unsigned edge = 0; edge < flow.count; ++edge) {
      if (flow.direct[edge] > operation.pc) { forward_.insert(flow.direct[edge]); }
    }
    if (i == 0 || instructions[i - 1].pc + instructions[i - 1].instruction.length != operation.pc) {
      heads_.insert(operation.pc);
    }
    const auto next = static_cast<Address>(operation.pc + operation.instruction.length);
    if (flow.indirect || flow.count != 1 || flow.direct[0] != next) {
      heads_.insert(next);
      for (unsigned edge = 0; edge < flow.count; ++edge) { heads_.insert(flow.direct[edge]); }
    }
  }
}

void Grants::summarize(const std::span<const BlockInstruction> instructions) {
  for (auto i = instructions.size(); i != 0; --i) {
    const auto &operation = instructions[i - 1];
    const auto timing = instruction_timing(operation.instruction);
    GrantCost after;
    if (operation.instruction.kind != Kind::call) {
      const auto flow = successors(operation);
      if (!flow.indirect) {
        for (unsigned edge = 0; edge < flow.count; ++edge) {
          const auto successor = ahead_.find(flow.direct[edge]);
          if (flow.direct[edge] > operation.pc && successor != ahead_.end() &&
              !loops_.contains(flow.direct[edge])) {
            after.instructions = std::max(after.instructions, successor->second.instructions);
            after.cycles = std::max(after.cycles, successor->second.cycles);
          }
        }
      }
    }
    ahead_.emplace(operation.pc,
                   GrantCost{.instructions = after.instructions + 1,
                             .cycles = after.cycles + timing.base + timing.taken_extra});
    if (after.instructions == 0 && heads_.contains(operation.pc) &&
        !forward_.contains(operation.pc) && !loops_.contains(operation.pc)) {
      single_.insert(operation.pc);
    }
  }
}

void Grants::capture(const std::span<const BlockInstruction> instructions,
                     const std::size_t first,
                     const std::size_t last) {
  GrantCost cost;
  for (auto i = first; i <= last; ++i) {
    const auto &current = instructions[i];
    const auto timing = instruction_timing(current.instruction);
    ++cost.instructions;
    cost.cycles += timing.base + timing.taken_extra;
    if (i != first && heads_.contains(current.pc)) { return; }
    if (i == last) { continue; }
    const auto flow = successors(current);
    if (timing.taken_extra != 0 || flow.indirect || flow.count != 1 ||
        flow.direct[0] != instructions[i + 1].pc || current.instruction.kind == Kind::call) {
      return;
    }
  }
  loops_.emplace(instructions[first].pc, LoopGrant{.first = first, .last = last, .cost = cost});
}
}
