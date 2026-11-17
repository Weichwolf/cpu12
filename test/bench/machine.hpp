// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "measurement.hpp"
#include "program.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpu12::bench {
inline constexpr Address stack_top = 0x7000, vector = 0xfff0;

inline void require(const bool condition, const char *reason) {
  if (!condition) { throw std::runtime_error(reason); }
}

inline void put(std::span<Byte> ram, const Address address, const unsigned value) {
  ram[address] = static_cast<Byte>(value >> byte_bits);
  ram[static_cast<Address>(address + 1)] = static_cast<Byte>(value);
}

inline unsigned get(std::span<const Byte> ram, const Address address) {
  return (unsigned{ram[address]} << byte_bits) | ram[static_cast<Address>(address + 1)];
}

// All kernels use the contract's installed-image public execution path.
struct Machine {
  explicit Machine(Program source, const bool irq = false) : program(std::move(source)) {
    std::ranges::copy(program.code(), ram.begin() + code_base);
    put(ram, vector, irq ? program.at("irq") : code_base);
    const auto code_end = static_cast<Address>(code_base + program.code().size());
    regions = {Ram{.bytes = std::span(ram).first(code_base)},
               Ram{.first = code_base,
                   .bytes = std::span(ram).subspan(code_base, program.code().size()),
                   .writable = false},
               Ram{.first = code_end, .bytes = std::span(ram).subspan(code_end, vector - code_end)},
               Ram{.first = vector, .bytes = std::span(ram).subspan(vector, 2), .writable = false},
               Ram{.first = vector + 2, .bytes = std::span(ram).subspan(vector + 2)}};
    std::vector<RegionLayout> layout;
    layout.reserve(regions.size());
    for (const auto &region : regions) {
      layout.push_back(
          {.first = region.first, .size = region.bytes.size(), .writable = region.writable});
    }
    const std::array spans{CodeSpan{.address = code_base, .words = program.code()}};
    std::vector<Address> entries(program.entries().begin(), program.entries().end());
    if (irq) { entries.push_back(program.at("irq")); }
    const std::array variants{CodeVariant{.id = 0, .code = spans, .entries = entries}};
    const std::array layouts{VariantLayout{.id = 0, .memory = layout}};
    std::vector<InterruptRequest> interrupts;
    if (irq) { interrupts.push_back({.vector = vector}); }
    const auto start = Clock::now();
    image = Core::compile({.variants = variants},
                          {.memory = layouts,
                           .interrupts = interrupts,
                           .transitions = {},
                           .callbacks = {},
                           .bindings = {}});
    core.set_memory(Memory::mapped(regions));
    core.install(image, 0);
    preparation = std::chrono::duration<double>(Clock::now() - start).count();
    initial = core.save();
  }

  Program program;
  std::array<Byte, address_bytes> ram{};
  std::array<Ram, 5> regions;
  Core core;
  NativeImage image;
  Snapshot initial;
  double preparation = 0;
};
}
