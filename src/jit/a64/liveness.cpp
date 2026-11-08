// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"
#include "jit/resident.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cpu12::detail::a64 {
bool Emitter::resident_y_live(const ResidentPlan &plan,
                              const std::size_t index,
                              const bool live,
                              const std::span<const StoreMode> stores) const {
  for (auto at = index + 1; at < plan.operations.size(); ++at) {
    const auto &operation = plan.operations[at];
    const auto &instruction = operation.instruction;
    const auto kind = instruction.kind;
    if (kind == Kind::nop || kind == Kind::branch) { continue; }
    if (instruction.addressing == Addressing::indexed || operation.forwarded_word == 2 ||
        operation.initial_load || kind == Kind::multiply_divide) {
      return true;
    }
    if (!instruction.word || instruction.target != WordTarget::y) { continue; }
    if (kind == Kind::load) { return false; }
    const bool dead_store = kind == Kind::store && stores[at] == StoreMode::elided &&
                            (!live || operation.flags == 0) && !operation.cache_store &&
                            resident_word(operation) == Reg::zr && operation.value_slot == 0;
    if (!dead_store) { return true; }
  }
  return true;
}

void Emitter::resident_inputs(const ResidentPlan &plan) {
  constexpr unsigned d_bit = 1U << 3U;
  constexpr unsigned y_bit = 1U << 2U;
  unsigned defined = 0;
  unsigned read = 0;
  for (const auto &operation : plan.operations) {
    const auto &instruction = operation.instruction;
    const auto kind = instruction.kind;
    if (kind == Kind::nop || kind == Kind::branch) { continue; }
    const auto target = 1U << native_word(instruction);
    if (kind == Kind::load && instruction.word) {
      defined |= target;
    } else if (kind == Kind::multiply_divide) {
      read |= (d_bit | y_bit) & ~defined;
      defined |= d_bit | y_bit;
    } else {
      read |= target & ~defined;
    }
  }
  const auto overwritten = defined & ~read;
  constexpr std::array words{std::pair{d_bit, d}, std::pair{y_bit, y}};
  for (const auto &[word, host] : words) {
    if ((overwritten & word) != 0) { input_registers_ &= ~(1U << static_cast<unsigned>(host)); }
  }
  input_registers_ &= ~(1U << static_cast<unsigned>(pc));
}

bool Emitter::resident_deferred(const ResidentOperation &operation) const {
  return std::ranges::find(head_stores_, operation.pointer) != head_stores_.end() ||
         std::ranges::find(tail_stores_, operation.pointer) != tail_stores_.end();
}

namespace {
enum class WordWrites : std::uint8_t { none, head, tail };

WordWrites word_writes(const ResidentPlan &plan, const Byte *pointer, const std::size_t head) {
  const auto word = reinterpret_cast<std::uintptr_t>(pointer);
  auto result = WordWrites::none;
  for (std::size_t index = 0; index < plan.operations.size(); ++index) {
    const auto &operation = plan.operations[index];
    if (operation.pointer == nullptr) { continue; }
    const auto address = reinterpret_cast<std::uintptr_t>(operation.pointer);
    const auto last = address + (operation.instruction.word ? 1U : 0U);
    if (last < word || word + 1 < address) { continue; }
    if (address != word || !operation.instruction.word) { return WordWrites::none; }
    if (operation.instruction.kind == Kind::store && result != WordWrites::head) {
      result = index < head ? WordWrites::head : WordWrites::tail;
    }
  }
  return result;
}
}

void Emitter::resident_region_stores(const ResidentPlan &plan, const std::size_t head) {
  for (const auto *pointer : resident_words_) {
    const auto writes = word_writes(plan, pointer, head);
    if (writes == WordWrites::none) { continue; }
    (writes == WordWrites::head ? head_stores_ : tail_stores_).push_back(pointer);
  }
}

std::vector<StoreMode> Emitter::resident_stores(const ResidentPlan &plan) const {
  std::vector<StoreMode> result(plan.operations.size(), StoreMode::write);
  if (!plan.indexed_regions.empty()) { return result; }
  std::unordered_set<std::uintptr_t> covered;
  covered.reserve(plan.operations.size() * 2);
  for (auto index = plan.operations.size(); index != 0; --index) {
    const auto &operation = plan.operations[index - 1];
    if (operation.pointer == nullptr) { continue; }
    const bool write = operation.instruction.kind == Kind::store;
    const bool forwarded = operation.initial_load || operation.forwarded_word != 0 ||
                           operation.cached_operand || resident_word(operation) != Reg::zr ||
                           (operation.value_slot != 0 && !operation.fill_value);
    if (!write && forwarded) { continue; }
    const auto address = reinterpret_cast<std::uintptr_t>(operation.pointer);
    const auto bytes = operation.instruction.word ? 2U : 1U;
    bool overwritten = true;
    for (unsigned byte = 0; byte < bytes; ++byte) {
      overwritten = overwritten && covered.contains(address + byte);
    }
    if (write && overwritten) { result[index - 1] = StoreMode::elided; }
    for (unsigned byte = 0; byte < bytes; ++byte) {
      if (write) {
        covered.insert(address + byte);
      } else {
        covered.erase(address + byte);
      }
    }
  }
  return result;
}
}
