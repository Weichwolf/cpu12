// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::interrupts {
inline constexpr std::array<Byte, 1> rti{0x0b};
inline constexpr Registers seed{
    .pc = 0x200, .d = 0xf00d, .x = 0xdead, .y = 0xbeef, .sp = 0xfffc, .ccr = 0x31};
inline constexpr Registers restored{
    .pc = 0xdef0, .d = 0x1234, .x = 0x5678, .y = 0x9abc, .sp = 5, .ccr = 0xd7};
inline constexpr std::array<Byte, 9> literal{0xd7, 0x34, 0x12, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
using Image = std::array<Byte, address_bytes>;
inline std::uint64_t checked = 0;

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline Registers expected(const Registers before, Registers target) {
  target.sp = static_cast<Address>(before.sp + 9U);
  constexpr unsigned other_flags = 0xbf;
  constexpr unsigned x = 0x40;
  target.ccr = static_cast<Byte>((target.ccr & other_flags) | (target.ccr & before.ccr & x));
  return target;
}

inline std::array<Byte, 9> encode(const Registers target) {
  constexpr unsigned byte_bits = 8;
  constexpr unsigned mask = 0xff;
  return {target.ccr,
          static_cast<Byte>(target.d & mask),
          static_cast<Byte>(target.d >> byte_bits),
          static_cast<Byte>(target.x >> byte_bits),
          static_cast<Byte>(target.x & mask),
          static_cast<Byte>(target.y >> byte_bits),
          static_cast<Byte>(target.y & mask),
          static_cast<Byte>(target.pc >> byte_bits),
          static_cast<Byte>(target.pc & mask)};
}

inline void put(Image &image, const Address sp, const std::array<Byte, 9> frame) {
  for (unsigned offset = 0; offset < frame.size(); ++offset) {
    image[static_cast<Address>(sp + offset)] = frame[offset];
  }
}

inline void accept(Core &core, const Registers before, const Registers wanted) {
  const auto result = core.run_instruction(rti);
  require(result.retired == 1 && result.fault == Fault::none && result.pc == before.pc &&
              result.opcode == rti[0] && core.registers() == wanted,
          "RTI must restore independently specified registers, B:A order, X mask and wrapped SP");
  ++checked;
}

void bus_cases();
}
