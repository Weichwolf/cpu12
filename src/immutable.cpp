// SPDX-License-Identifier: GPL-3.0-only
#include "immutable.hpp"

#include "cpu12/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace cpu12::detail {
std::span<const Byte> immutable_bytes(const Memory &memory, const Address address) {
  for (const auto &region : memory.regions()) {
    if (region.writable || !region.known.empty() || address < region.first) { continue; }
    const auto offset = static_cast<std::size_t>(address - region.first);
    if (offset >= region.bytes.size()) { continue; }
    const auto *pointer = region.bytes.data() + offset;
    auto remaining = region.bytes.size() - offset;
    const auto physical = reinterpret_cast<std::uintptr_t>(pointer);
    for (const auto &alias : memory.regions()) {
      if (!alias.writable || alias.bytes.empty()) { continue; }
      const auto first = reinterpret_cast<std::uintptr_t>(alias.bytes.data());
      if (physical >= first && physical - first < alias.bytes.size()) { return {}; }
      if (first > physical && first - physical < remaining) {
        remaining = static_cast<std::size_t>(first - physical);
      }
    }
    return {pointer, remaining};
  }
  return {};
}
}
