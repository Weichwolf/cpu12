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

namespace cpu12::test::native_flow {
constexpr Address entry = 0x4000, first = 0x4100, second = 0x4200, nested = 0x4300, idle = 0x4500;
constexpr Address last = 0xffff;
constexpr Address code_end = 0x4600, x_value = 0x3456;
constexpr Byte a_value = 0x7f, b_value = 0x81, preserved = 0xf1;
constexpr unsigned byte_bits = 8, byte_mask = 0xff, high_mask = 0xff00;
constexpr std::uint64_t variant_id = 1;

consteval auto call(const Address target) {
  return isa::program(
      isa::opcode<isa::Opcode::jsr_extended>(),
      std::array<Byte, 2>{static_cast<Byte>(target >> byte_bits), static_cast<Byte>(target)});
}

constexpr auto root_code =
    isa::program(call(first),
                 call(second),
                 std::array<Byte, 2>{static_cast<Byte>(isa::Opcode::bra_relative), 0xf8});
constexpr auto first_code = isa::program(
    isa::opcode<isa::Opcode::ldx_immediate>(),
    std::array<Byte, 2>{static_cast<Byte>(x_value >> byte_bits), static_cast<Byte>(x_value)},
    call(nested),
    isa::rts);
constexpr auto second_code = isa::program(
    isa::opcode<isa::Opcode::ldaa_immediate>(), std::array<Byte, 1>{a_value}, isa::rts);
constexpr auto nested_code = isa::program(
    isa::opcode<isa::Opcode::ldab_immediate>(), std::array<Byte, 1>{b_value}, isa::rts);
constexpr std::array<Byte, 2> idle_code{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
constexpr std::array spans{CodeSpan{.address = entry, .words = root_code},
                           CodeSpan{.address = first, .words = first_code},
                           CodeSpan{.address = second, .words = second_code},
                           CodeSpan{.address = nested, .words = nested_code},
                           CodeSpan{.address = idle, .words = idle_code}};
constexpr std::array entries{entry, idle};
constexpr std::array layout{
    RegionLayout{.first = 0, .size = entry},
    RegionLayout{.first = entry, .size = code_end - entry, .writable = false},
    RegionLayout{.first = code_end, .size = address_bytes - code_end}};
constexpr Registers seed{
    .pc = entry, .d = 0xabcd, .x = 0xdead, .y = 0xbeef, .sp = 0x9000, .ccr = 0xa5};

inline void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

struct Fixture {
  Fixture() {
    bytes.fill(isa::nop[0]);
    for (const auto &span : spans) { std::ranges::copy(span.words, bytes.begin() + span.address); }
    grants = {Ram{.bytes = std::span(bytes).first(entry)},
              Ram{.first = entry,
                  .bytes = std::span(bytes).subspan(entry, code_end - entry),
                  .writable = false},
              Ram{.first = code_end, .bytes = std::span(bytes).subspan(code_end)}};
  }

  [[nodiscard]] Memory memory() { return Memory::mapped(grants); }

  static bool read(void *owner, const Address at, Address &value) noexcept {
    if (at != last) { return false; }
    const auto &self = *static_cast<Fixture *>(owner);
    value = static_cast<Address>((unsigned{self.bytes[at]} << byte_bits) | self.bytes[0]);
    return true;
  }

  static bool write(void *owner, const WordWrite access) noexcept {
    if (access.address != last) { return false; }
    auto &self = *static_cast<Fixture *>(owner);
    self.bytes[access.address] = static_cast<Byte>(access.value >> byte_bits);
    self.bytes[0] = static_cast<Byte>(access.value);
    return true;
  }

  std::array<Byte, address_bytes> bytes{};
  std::array<Ram, layout.size()> grants{};
};

inline NativeImage compile() {
  const std::array variants{CodeVariant{.id = variant_id, .code = spans, .entries = entries}};
  const std::array layouts{VariantLayout{.id = variant_id, .memory = layout}};
  return Core::compile({.variants = variants},
                       {.memory = layouts, .interrupts = {}, .transitions = {}});
}

struct Model {
  Registers registers;
  std::array<Byte, address_bytes> bytes;
  std::uint64_t cycles = 0;
  std::uint32_t retired = 0;
  Address last_pc = 0;
  Byte last_opcode = 0;
  void step();
};

void reuse();
}
