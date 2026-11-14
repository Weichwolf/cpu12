// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/llvm/memory.hpp"
#include "jit/prefixes.hpp"
#include "jit/preparation.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpu12::detail {
unsigned operand_access_class(const Instruction &instruction) {
  const bool unary = instruction.kind == Kind::memory_unary;
  unsigned mode = 0;
  if (instruction.kind == Kind::store || (unary && instruction.operation == RegisterOp::clear)) {
    mode = 2;
  } else if (instruction.kind == Kind::memory_bits ||
             (unary && instruction.operation != RegisterOp::test)) {
    mode = 4;
  }
  return mode + (instruction.word ? 1U : 0U);
}

namespace {
bool preserves_indices(const Instruction &instruction) {
  const auto kind = instruction.kind;
  if (instruction.indexed.update != 0 || kind == Kind::transfer ||
      kind == Kind::effective_address || kind == Kind::multiply_divide || kind == Kind::move ||
      kind == Kind::push || kind == Kind::pull || kind == Kind::call ||
      kind == Kind::return_subroutine || kind == Kind::return_interrupt ||
      kind == Kind::software_interrupt || kind == Kind::wait || kind == Kind::stop) {
    return false;
  }
  if (kind == Kind::register_operation) {
    return instruction.operation != RegisterOp::index_increment &&
           instruction.operation != RegisterOp::index_decrement;
  }
  if (kind == Kind::loop) {
    return instruction.operation == RegisterOp::test || instruction.target == WordTarget::d;
  }
  const bool writes = kind == Kind::load || kind == Kind::add || kind == Kind::add_carry ||
                      kind == Kind::subtract || kind == Kind::subtract_carry ||
                      kind == Kind::bit_and || kind == Kind::bit_or || kind == Kind::bit_xor;
  return !writes || !instruction.word || instruction.target == WordTarget::d;
}

bool overlaps(const std::span<const Byte> left, const std::span<const Byte> right) {
  if (left.empty() || right.empty()) { return false; }
  const auto first = std::bit_cast<std::uintptr_t>(left.data());
  const auto second = std::bit_cast<std::uintptr_t>(right.data());
  return first <= second ? second - first < left.size() : first - second < right.size();
}

unsigned index_value(const Registers &registers, const IndexBase base) {
  switch (base) {
    case IndexBase::x: return registers.x;
    case IndexBase::y: return registers.y;
    case IndexBase::sp: return registers.sp;
    case IndexBase::pc:
    default: return 0;
  }
}

unsigned local_classes(const LocalAccess::Role role) {
  constexpr unsigned read = 3;
  constexpr unsigned write = 12;
  constexpr unsigned read_write = 63;
  switch (role) {
    case LocalAccess::Role::read: return read;
    case LocalAccess::Role::write: return write;
    case LocalAccess::Role::read_write: return read_write;
    default: throw std::invalid_argument("Invalid CPU12 isolated access role");
  }
}

std::vector<std::size_t> components(const std::span<const BlockInstruction> instructions,
                                    const std::map<Address, std::uint64_t> &prefixes,
                                    const std::span<const PrefixAccess> policies) {
  std::map<Address, std::size_t> indices;
  std::vector<std::size_t> parents;
  parents.reserve(instructions.size());
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    indices.emplace(instructions[index].pc, index);
    parents.push_back(index);
  }
  const auto root = [&](std::size_t index) {
    while (parents[index] != index) {
      parents[index] = parents[parents[index]];
      index = parents[index];
    }
    return index;
  };
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    if (prefixes.at(instructions[index].pc) == 0 || policies[index] == PrefixAccess::terminal) {
      continue;
    }
    const auto flow = successors(instructions[index]);
    for (const auto pc : std::span(flow.direct).first(flow.count)) {
      const auto next = indices.find(pc);
      if (next != indices.end() && prefixes.at(pc) != 0) {
        parents[root(index)] = root(next->second);
      }
    }
  }
  for (std::size_t index = 0; index < parents.size(); ++index) { parents[index] = root(index); }
  return parents;
}

}

GuardedPrefixes::GuardedPrefixes(const std::span<const BlockInstruction> instructions,
                                 const Memory &memory,
                                 const std::span<const LocalAccess> local) {
  bind_local(local);
  for (const auto &region : memory.regions()) {
    if (std::ranges::any_of(memory.regions(), [&](const auto &other) {
          return overlaps(region.bytes, other.known);
        })) {
      return;
    }
  }
  bind_memory(memory);
  prepare(instructions, memory);
}

void GuardedPrefixes::bind_memory(const Memory &memory) {
  for (unsigned access = 0; access < ranges_.size(); ++access) {
    const bool word = (access & 1U) != 0;
    const bool write = access >= 2;
    const bool read = access < 2 || access >= 4;
    const unsigned width = word ? 2U : 1U;
    for (const auto &region : memory.regions()) {
      if (region.bytes.size() < width || (write && !region.writable)) { continue; }
      const bool wrapping = region.first == 0 && region.bytes.size() == address_bytes;
      const auto last = region.first + region.bytes.size() - (wrapping ? 1U : width);
      ranges_[access].push_back(
          {.first = region.first,
           .last = static_cast<unsigned>(last),
           .known = read && !region.known.empty() ? region.known.data() : nullptr});
    }
  }
}

void GuardedPrefixes::bind_local(const std::span<const LocalAccess> local) {
  for (const auto region : local) {
    if (region.size == 0 || region.size > address_bytes - region.first) {
      throw std::invalid_argument("Invalid CPU12 isolated access extent");
    }
    const auto classes = local_classes(region.role);
    if (region.role != LocalAccess::Role::read) { store_policy_ = PrefixAccess::terminal; }
    for (unsigned access = 0; access < ranges_.size(); ++access) {
      const unsigned width = (access & 1U) + 1;
      if ((classes & (1U << access)) == 0 || region.size < width) { continue; }
      const bool wrapping = region.first == 0 && region.size == address_bytes;
      ranges_[access].push_back(
          {.first = region.first,
           .last = static_cast<unsigned>(region.first + region.size - (wrapping ? 1U : width)),
           .known = nullptr});
    }
  }
}

std::optional<GuardedPrefixes::Guard>
GuardedPrefixes::guard(const BlockInstruction &operation) const {
  const auto &instruction = operation.instruction;
  if (!llvm19::memory_operand(instruction) || instruction.addressing == Addressing::immediate) {
    return {};
  }
  const auto access = operand_access_class(instruction);
  if (ranges_[access].empty()) { return {}; }
  Guard result{.base = IndexBase::pc, .offset = instruction.address, .access = access};
  if (instruction.addressing == Addressing::indexed) {
    const auto &indexed = instruction.indexed;
    if (indexed.indirect || indexed.offset != IndexOffset::constant) { return {}; }
    result.base = indexed.base;
    result.offset =
        indexed.base == IndexBase::pc
            ? static_cast<Address>(operation.pc + instruction.length + indexed.displacement)
            : indexed.displacement;
  }
  if (result.base == IndexBase::pc && !std::ranges::any_of(ranges_[access], [&](const auto &range) {
        return result.offset >= range.first && result.offset <= range.last;
      })) {
    return {};
  }
  return result;
}

void GuardedPrefixes::prepare(const std::span<const BlockInstruction> instructions,
                              const Memory &memory) {
  std::vector<PrefixAccess> policies(instructions.size(), PrefixAccess::ordinary);
  std::vector<std::optional<Guard>> guards(instructions.size());
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto &operation = instructions[index];
    if (!preserves_indices(operation.instruction)) {
      policies[index] = PrefixAccess::boundary;
    } else if (const auto candidate = guard(operation)) {
      policies[index] = candidate->access >= 2 ? store_policy_ : PrefixAccess::guarded;
      guards[index] = candidate;
    }
  }
  const auto prefixes = foreground_prefixes(instructions, memory, policies);
  bind(instructions, prefixes, guards, policies);
}

void GuardedPrefixes::bind(const std::span<const BlockInstruction> instructions,
                           const std::map<Address, std::uint64_t> &prefixes,
                           const std::span<const std::optional<Guard>> guards,
                           const std::span<const PrefixAccess> policies) {
  const auto roots = components(instructions, prefixes, policies);
  std::map<std::size_t, std::vector<Guard>> groups;
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto &guard = guards[index];
    if (guard && prefixes.at(instructions[index].pc) != 0) {
      groups[roots[index]].push_back(*guard);
    }
  }
  groups_.emplace_back();
  std::map<std::size_t, std::size_t> group_ids;
  for (auto &[id, group] : groups) {
    std::ranges::sort(group);
    const auto unique = std::ranges::unique(group);
    group.erase(unique.begin(), unique.end());
    group_ids.emplace(id, groups_.size());
    groups_.push_back(std::move(group));
  }
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto pc = instructions[index].pc;
    const auto cycles = prefixes.at(pc);
    if (cycles == 0) { continue; }
    const auto found = group_ids.find(roots[index]);
    entries_.emplace(
        pc, Entry{.cycles = cycles, .group = found == group_ids.end() ? 0 : found->second});
  }
}

std::uint64_t GuardedPrefixes::prefix(const Address pc, const Registers &registers) const {
  const auto entry = entries_.find(pc);
  if (entry == entries_.end()) { return 0; }
  for (const auto &guard : groups_[entry->second.group]) {
    const auto at = static_cast<Address>(index_value(registers, guard.base) + guard.offset);
    const auto next = static_cast<Address>(at + 1U);
    const bool word = (guard.access & 1U) != 0;
    if (!std::ranges::any_of(ranges_[guard.access], [&](const auto &range) {
          return at >= range.first && at <= range.last &&
                 (range.known == nullptr || (range.known[at - range.first] != 0 &&
                                             (!word || range.known[next - range.first] != 0)));
        })) {
      return 0;
    }
  }
  return entry->second.cycles;
}
}
