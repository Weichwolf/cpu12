// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::irqs {
using Image = std::array<Byte, address_bytes>;
inline constexpr Registers seed{
    .pc = 0x2222, .d = 0x1234, .x = 0x5678, .y = 0x9abc, .sp = 0x8000, .ccr = 0x20};
inline constexpr Address vector = 0xfff2, handler = 0x400;
inline std::uint64_t entered = 0;

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline Registers expected(const Registers before, const InterruptKind kind) {
  auto result = before;
  result.pc = handler;
  result.sp = static_cast<Address>(before.sp + address_bytes - 9U);
  constexpr unsigned irq = 0x10;
  constexpr unsigned xirq = 0x50;
  result.ccr = static_cast<Byte>(before.ccr | (kind == InterruptKind::xirq ? xirq : irq));
  return result;
}

inline void check_frame(const Image &image, const Registers before) {
  constexpr unsigned byte_bits = 8;
  constexpr unsigned mask = 0xff;
  const std::array<Byte, 9> frame{before.ccr,
                                  static_cast<Byte>(before.d & mask),
                                  static_cast<Byte>(before.d >> byte_bits),
                                  static_cast<Byte>(before.x >> byte_bits),
                                  static_cast<Byte>(before.x & mask),
                                  static_cast<Byte>(before.y >> byte_bits),
                                  static_cast<Byte>(before.y & mask),
                                  static_cast<Byte>(before.pc >> byte_bits),
                                  static_cast<Byte>(before.pc & mask)};
  for (unsigned offset = 0; offset < frame.size(); ++offset) {
    const auto address = static_cast<Address>(before.sp + address_bytes - 9U + offset);
    require(image[address] == frame[offset],
            "IRQ frame contains the original CCR,B,A,X,Y and next PC at independent byte offsets");
  }
}

inline void prepare_vector(Image &image, const Address at) {
  image[at] = 4;
  image[static_cast<Address>(at + 1U)] = 0;
}

inline void
accept(Core &core, const Registers before, const InterruptRequest request, const Image &image) {
  const auto result = core.interrupt(request);
  require(
      result.accepted && result.fault == Fault::none && result.pc == before.pc &&
          core.registers() == expected(before, request.kind),
      "Accepted IRQ reads a real vector, saves actual context and applies only interrupt masks");
  check_frame(image, before);
  ++entered;
}

void bus_cases();
}
