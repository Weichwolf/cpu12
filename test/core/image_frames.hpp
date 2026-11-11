// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace cpu12::test::image_frames {
constexpr Address entry = 0x4000, vector = 0xfff2;
constexpr Address last = 0xffff;
constexpr Address wai = entry + 1, stop = entry + 5, rti = entry + 10;
constexpr unsigned byte_bits = 8, byte_mask = 0xff;
constexpr std::uint64_t variant_id = 1;
constexpr Byte guard = 0x5a;
constexpr auto loop = std::array<Byte, 2>{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
constexpr auto code = isa::program(isa::nop,
                                   isa::wai,
                                   loop,
                                   isa::nop,
                                   isa::stop,
                                   loop,
                                   isa::nop,
                                   isa::rti,
                                   isa::nop,
                                   loop,
                                   isa::opcode<isa::Opcode::cli_immediate>(),
                                   isa::rti);
constexpr std::array full{
    RegionLayout{.first = 0, .size = entry},
    RegionLayout{.first = entry, .size = code.size(), .writable = false},
    RegionLayout{.first = entry + code.size(), .size = address_bytes - entry - code.size()}};
constexpr std::array external{RegionLayout{.first = entry, .size = code.size(), .writable = false}};
constexpr std::array partial{RegionLayout{.first = 0, .size = 8, .tracked = true}, external[0]};
constexpr std::array requests{InterruptRequest{.vector = vector},
                              InterruptRequest{.vector = vector, .kind = InterruptKind::xirq}};
constexpr Registers seed{.pc = wai, .d = 0x1234, .x = 0x5678, .y = 0x9abc, .sp = 4, .ccr = 0x20};

struct Form {
  Address pc;
  Byte opcode;
  ExecutionState execution;
  unsigned cycles;
};

constexpr std::array forms{
    Form{.pc = wai, .opcode = isa::wai[0], .execution = ExecutionState::waiting, .cycles = 7},
    Form{.pc = stop, .opcode = isa::stop[0], .execution = ExecutionState::stopped, .cycles = 8},
    Form{.pc = rti, .opcode = isa::rti[0], .execution = ExecutionState::running, .cycles = 8}};

inline void require(const bool accepted, const char *message) {
  if (!accepted) { throw std::runtime_error(message); }
}

inline NativeImage compile(const std::span<const RegionLayout> layout) {
  const std::array spans{CodeSpan{.address = entry, .words = code}};
  constexpr std::array entries{
      entry, static_cast<Address>(entry + 4), static_cast<Address>(entry + 9)};
  const std::array variants{CodeVariant{.id = variant_id, .code = spans, .entries = entries}};
  const std::array layouts{VariantLayout{.id = variant_id, .memory = layout}};
  return Core::compile({.variants = variants}, {.memory = layouts, .interrupts = requests});
}

struct Fixture {
  explicit Fixture(const std::span<const RegionLayout> layout) : count(layout.size()) {
    bytes.fill(guard);
    std::ranges::copy(code, bytes.begin() + entry);
    for (std::size_t index = 0; index < count; ++index) {
      const auto &region = layout[index];
      grants[index] = {.first = region.first,
                       .bytes = std::span(bytes).subspan(region.first, region.size),
                       .known = region.tracked ? std::span(known).subspan(region.first, region.size)
                                               : std::span<Byte>{},
                       .writable = region.writable};
    }
  }

  [[nodiscard]] Memory memory() { return Memory::mapped(std::span(grants).first(count)); }

  static bool read_word(void *owner, const Address at, Address &value) noexcept {
    if (at != last) { return false; }
    const auto &self = *static_cast<Fixture *>(owner);
    value = static_cast<Address>((unsigned{self.bytes[at]} << byte_bits) |
                                 self.bytes[static_cast<Address>(at + 1U)]);
    return true;
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    if (access.address != last) { return false; }
    auto &self = *static_cast<Fixture *>(owner);
    self.bytes[access.address] = static_cast<Byte>(access.value >> byte_bits);
    self.bytes[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value);
    return true;
  }

  std::array<Byte, address_bytes> bytes{}, known{};
  std::array<Ram, full.size()> grants{};
  std::size_t count;
};

[[nodiscard]] inline Registers sleeping(Registers before) {
  before.pc = static_cast<Address>(before.pc + (before.pc == stop ? 2U : 1U));
  before.sp = static_cast<Address>(unsigned{before.sp} + address_bytes - 9U);
  return before;
}

void bus_cases();
void deadlines(Core &core, Fixture &fixture);
}
