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
#include <string_view>

namespace cpu12::test::transitions {
constexpr Address start = 0x4000, destination = 0x5000, vector = 0xfff2;
constexpr unsigned byte_bits = 8, nzv = 0x0e;
constexpr std::uint64_t from = 7, to = 9;
constexpr std::array<Byte, 2> loop{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
constexpr auto payload =
    isa::program(isa::opcode<isa::Opcode::ldab_immediate>(), std::array<Byte, 1>{0x7f}, loop);
constexpr std::array<Address, 1> roots{start}, targets{destination};
constexpr std::array requests{InterruptRequest{.vector = vector}};
constexpr std::array forward{CodeTransition{.from = from, .to = to, .entry = destination}};
constexpr Registers seed{
    .pc = start, .d = 0x1234, .x = destination, .y = 0xbeef, .sp = 0x9000, .ccr = 0xa5};

constexpr auto jump(const Address at, const bool call = false) {
  return std::array<Byte, 3>{
      static_cast<Byte>(call ? isa::Opcode::jsr_extended : isa::Opcode::jmp_extended),
      static_cast<Byte>(at >> byte_bits),
      static_cast<Byte>(at)};
}

consteval auto store(const Byte value, const Address at) {
  return isa::program(
      isa::opcode<isa::Opcode::movb_immediate_to_extended>(),
      std::array<Byte, 3>{value, static_cast<Byte>(at >> byte_bits), static_cast<Byte>(at)});
}

constexpr auto loader = isa::program(store(payload[0], destination),
                                     store(payload[1], destination + 1),
                                     store(payload[2], destination + 2),
                                     store(payload[3], destination + 3),
                                     jump(destination));
using Bytes = std::span<const Byte>;

inline auto layout(const Bytes code, const Bytes next, const bool ready) {
  return std::array{
      RegionLayout{.first = 0, .size = start},
      RegionLayout{.first = start, .size = code.size(), .writable = false},
      RegionLayout{.first = static_cast<Address>(start + code.size()),
                   .size = destination - start - code.size()},
      RegionLayout{
          .first = destination, .size = next.size(), .writable = !ready, .tracked = !ready},
      RegionLayout{.first = static_cast<Address>(destination + next.size()),
                   .size = address_bytes - destination - next.size()}};
}

inline NativeImage compile(const Bytes code = loader,
                           const Bytes next = payload,
                           const std::span<const CodeTransition> transitions = forward,
                           const std::span<const Address> entries = roots) {
  const std::array first{CodeSpan{.address = start, .words = code}};
  const std::array second{CodeSpan{.address = destination, .words = next}};
  const std::array variants{CodeVariant{.id = from, .code = first, .entries = entries},
                            CodeVariant{.id = to, .code = second, .entries = targets}};
  const auto original = layout(code, next, false);
  const auto loaded = layout(code, next, true);
  const std::array layouts{VariantLayout{.id = from, .memory = original},
                           VariantLayout{.id = to, .memory = loaded}};
  return Core::compile({.variants = variants},
                       {.memory = layouts,
                        .interrupts = requests,
                        .transitions = transitions,
                        .callbacks = {},
                        .bindings = {}});
}

inline void require(const bool value, const char *reason) {
  if (!value) { throw std::runtime_error(reason); }
}

template <class Action> void rejects(Action action, const std::string_view reason) {
  bool rejected = false;
  try {
    action();
  } catch (const std::exception &error) {
    rejected = std::string_view(error.what()).contains(reason);
  }
  require(rejected, "A rejected transition operation reports its actual contract violation");
}

struct Fixture {
  explicit Fixture(const Bytes source = loader,
                   const Bytes next = payload,
                   const bool loaded = false)
      : source_size(source.size()), target_size(next.size()) {
    std::ranges::copy(source, bytes.begin() + start);
    if (loaded) {
      std::ranges::copy(next, bytes.begin() + destination);
      std::fill_n(known.begin() + destination, next.size(), Byte{1});
    }
    bytes[vector] = static_cast<Byte>(destination >> byte_bits);
    bytes[vector + 1] = static_cast<Byte>(destination);
  }

  Memory memory(const bool ready = false) {
    const auto requested = layout(Bytes(bytes).subspan(start, source_size),
                                  Bytes(bytes).subspan(destination, target_size),
                                  ready);
    for (std::size_t i = 0; i < grants.size(); ++i) {
      const auto &region = requested[i];
      grants[i] = {.first = region.first,
                   .bytes = std::span(bytes).subspan(region.first, region.size),
                   .known = region.tracked ? std::span(known).subspan(region.first, region.size)
                                           : std::span<Byte>{},
                   .writable = region.writable};
    }
    return Memory::mapped(grants);
  }

  std::array<Byte, address_bytes> bytes{}, known{};
  std::array<Ram, 5> grants{};
  std::size_t source_size, target_size;
};

void contracts();
void transfers();
void state_boundaries();
}
