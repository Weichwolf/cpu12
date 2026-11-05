// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"

#include <cstddef>

namespace cpu12::detail {
inline Ram plain_ram(const Memory &memory) noexcept {
  const auto regions = memory.regions();
  if (regions.size() != 1 || !regions[0].known.empty() || !regions[0].writable) { return {}; }
  return regions[0];
}

inline Byte *known_byte(const Memory &memory, const Address address) noexcept {
  for (const auto region : memory.regions()) {
    if (address < region.first) { continue; }
    const auto offset = static_cast<std::size_t>(address - region.first);
    if (offset < region.bytes.size()) {
      return region.known.empty() || region.known[offset] != 0 ? region.bytes.data() + offset
                                                               : nullptr;
    }
  }
  return nullptr;
}
}
