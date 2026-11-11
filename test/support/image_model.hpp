// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <cstdint>

namespace cpu12::test::image {
constexpr Byte negative = 0x80;
constexpr unsigned high_mask = 0xff00;
constexpr std::array<unsigned, 11> grants{0, 1, 2, 3, 4, 5, 6, 7, 8, 17, 128};
constexpr std::array<Byte, 8> flags{0, 1, 4, 8, 0x10, 0x40, 0x80, 0xff};
constexpr Registers initial{.pc = entry, .d = 0xa500, .x = 0x1234, .y = 0x5678, .sp = 0x9000};

struct Expected {
  Registers registers;
  std::uint64_t cycles = 0;
};

inline Expected predict(const Registers before, const unsigned grant, const Fixture &fixture) {
  Expected result{.registers = before};
  auto &r = result.registers;
  const auto offset = fixture.bytes[table + (before.d & byte_mask)];
  const auto destination = static_cast<Address>(table + offset);
  if (grant >= 1) {
    r.pc = helper;
    r.sp -= 2;
    result.cycles += 4;
  }
  if (grant >= 2) {
    r.pc = helper + 1;
    r.sp += 2;
    r.x = table;
    result.cycles += 3;
  }
  if (grant >= 3) {
    constexpr unsigned retained_flags = 0xf1;
    r.pc = helper + 3;
    r.d = static_cast<Address>((r.d & high_mask) | offset);
    r.ccr = static_cast<Byte>(r.ccr & retained_flags);
    result.cycles += 3;
  }
  if (grant >= 4) {
    r.pc = destination;
    result.cycles += 3;
  }
  if (grant >= 5) {
    const auto value = fixture.bytes[destination + 1];
    r.pc = static_cast<Address>(destination + 2);
    r.d = static_cast<Address>((static_cast<unsigned>(value) << byte_bits) | offset);
    r.ccr = static_cast<Byte>(r.ccr | (value == 0 ? 4U : 0U) | (value >= negative ? 8U : 0U));
    result.cycles += 1 + ((grant - 5U) * 3U);
  }
  return result;
}

}
