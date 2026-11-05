// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::resetting {
using Image = std::array<Byte, address_bytes>;
inline constexpr Registers seed{
    .pc = 0x2222, .d = 0x1234, .x = 0x5678, .y = 0x9abc, .sp = 0xdef0, .ccr = 0xa5};
inline constexpr Address vector = 0xfffe;
inline std::uint64_t accepted = 0;

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline Registers expected(const Registers before, const Address target) {
  auto result = before;
  result.pc = target;
  constexpr unsigned reset_masks = 0xd0;
  result.ccr = static_cast<Byte>(before.ccr | reset_masks);
  return result;
}

inline void accept(Core &core, const Address at, const Registers before, const Address target) {
  const auto result = core.reset(at);
  require(
      result.accepted && result.fault == Fault::none && result.pc == before.pc &&
          core.registers() == expected(before, target),
      "Reset reads the actual vector and forces S/X/I, retaining other state as explicit policy");
  ++accepted;
}

void bus_cases();
}
