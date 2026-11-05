// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::masked {
inline constexpr Registers seed{
    .pc = 0xfffd, .d = 0x1234, .x = 0xabcd, .y = 0x5678, .sp = 0xfffe, .ccr = 0xa5};
inline std::uint64_t checked = 0;

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

struct Bus {
  static constexpr Byte all_bits = 0xff;
  Byte value = 0, known = all_bits;
  unsigned selected = 0, full = 0, writes = 0;
  MaskedByteRead observed{};
  ByteWrite written{};
  bool allowed = true;
  bool reject_selected = false;

  static bool masked(void *owner, const MaskedByteRead access, Byte &result) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.selected;
    bus.observed = access;
    result = bus.value;
    bus.value = 0;
    const auto unknown =
        static_cast<unsigned>(access.mask) & (all_bits ^ static_cast<unsigned>(bus.known));
    return bus.allowed && !bus.reject_selected && unknown == 0;
  }

  static bool read(void *owner, const Address address, Byte &result) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.full;
    constexpr Byte full_mask = 0xff;
    bus.observed = {.address = address, .mask = full_mask};
    result = bus.value;
    bus.value = 0;
    return bus.allowed && bus.known == full_mask;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.writes;
    bus.written = access;
    return true;
  }
};

void boundary_cases();
}
