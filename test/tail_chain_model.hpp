// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::tail_chains {
using Image = std::array<Byte, address_bytes>;
constexpr Address frame_sp = 0x8000, vector = 0xfff2, target = 0x400;
constexpr Registers initial{
    .pc = 0x100, .d = 0xbeef, .x = 0xdead, .y = 0xf00d, .sp = frame_sp, .ccr = 0};
constexpr std::array<Byte, 9> literal{0x60, 0x34, 0x12, 0x56, 0x78, 0x9a, 0xbc, 0x24, 0x68};
constexpr std::array<Byte, 1> rti{0x0b}, nop{0xa7};
inline std::uint64_t chained = 0;

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline void put_frame(Image &image, const Address sp, const Byte ccr = literal[0]) {
  for (unsigned index = 0; index < literal.size(); ++index) {
    image[static_cast<Address>(sp + index)] = index == 0 ? ccr : literal[index];
  }
}

inline Registers restored(const Registers before, const Byte stacked = literal[0]) {
  constexpr unsigned x = 0x40;
  constexpr unsigned other = 0xbf;
  constexpr Registers decoded{.pc = 0x2468, .d = 0x1234, .x = 0x5678, .y = 0x9abc};
  auto result = decoded;
  result.sp = static_cast<Address>(before.sp + 9U);
  result.ccr = static_cast<Byte>((stacked & other) | (stacked & before.ccr & x));
  return result;
}

inline void restore_frame(Core &core, const Registers before, const Byte stacked = literal[0]) {
  core.set_registers(before);
  const auto result = core.run_instruction(rti);
  require(result.retired == 1 && result.fault == Fault::none && result.pc == before.pc &&
              core.registers() == restored(before, stacked) && core.interrupt_frame_available() &&
              core.checkpoint().returned_frame,
          "Native RTI restores the independently specified literal frame and exposes it at the "
          "boundary");
}

inline void accept(Core &core, const InterruptRequest request, const Address destination = target) {
  const auto before = core.registers();
  const auto retired = core.statistics().native_instructions;
  require(core.interrupt_frame_available() && core.interrupt_ready(request.kind),
          "Pending source is eligible at the just-returned architectural boundary");
  auto wanted = before;
  wanted.pc = destination;
  wanted.sp = static_cast<Address>(before.sp + address_bytes - 9U);
  constexpr unsigned irq = 0x10;
  constexpr unsigned xirq = 0x50;
  wanted.ccr = static_cast<Byte>(before.ccr | (request.kind == InterruptKind::xirq ? xirq : irq));
  const auto result = core.interrupt(request);
  require(result.accepted && result.fault == Fault::none && result.pc == before.pc &&
              core.registers() == wanted && !core.interrupt_frame_available() &&
              !core.checkpoint().returned_frame && core.statistics().native_instructions == retired,
          "Native post-RTI entry reads the vector, backs SP by9, applies masks and retires no "
          "instruction");
  ++chained;
}

void bus_cases();
void checkpoint_cases();
}
