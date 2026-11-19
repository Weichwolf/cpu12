// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address entry = 0x8000, data = 0x1000, stack = 0x7000;
constexpr unsigned mask = 0xffff, sign = 0x8000, byte_bits = 8;
constexpr unsigned steps = 40;
constexpr unsigned byte_mask = 0xff, high_flags = 0xf0, store_flags = 0xf1, deadline_bound = 100;
constexpr Byte initial_flags = 0xb1;
constexpr Address counted_initial = 0x7ffd, conditional_initial = 0xfff1;
constexpr std::array<Byte, 10> counted_code{0xc3, 0, 3, 0x6c, 0x31, 4, 0x36, 0xf8, 0x20, 0xfe};
constexpr std::array<Byte, 9> conditional_code{0xc3, 0, 3, 0x6c, 0x31, 0x26, 0xf9, 0x20, 0xfe};

void require(const bool good, const char *reason) {
  if (!good) { throw std::runtime_error(reason); }
}

Byte nz(const unsigned value) {
  return static_cast<Byte>((value == 0 ? 4U : 0U) | ((value & sign) != 0 ? 8U : 0U));
}

struct Prediction {
  Registers registers;
  std::array<Byte, address_bytes> ram;
  unsigned cycles = 0;
};

unsigned step(Prediction &p, const bool counted) {
  auto &r = p.registers;
  switch (r.pc - entry) {
    case 0: {
      const unsigned sum = unsigned{r.d} + 3;
      const unsigned overflow =
          ((~(unsigned{r.d} ^ 3U) & (unsigned{r.d} ^ sum)) & sign) != 0 ? 2U : 0U;
      r.d = static_cast<Address>(sum);
      r.ccr = static_cast<Byte>((r.ccr & high_flags) | nz(r.d) | overflow | (sum > mask ? 1U : 0U));
      r.pc += 3;
      return 2;
    }
    case 3:
      p.ram[r.x] = static_cast<Byte>(r.d >> byte_bits);
      p.ram[static_cast<Address>(r.x + 1)] = static_cast<Byte>(r.d);
      r.x += 2;
      r.ccr = static_cast<Byte>((r.ccr & store_flags) | nz(r.d));
      r.pc += 2;
      return 2;
    case 5: {
      if (counted) { --r.y; }
      const bool taken = counted ? r.y != 0 : (r.ccr & 4U) == 0;
      r.pc = taken ? entry : static_cast<Address>(entry + (counted ? 8 : 7));
      return counted || taken ? 3U : 1U;
    }
    default: return 3;
  }
}

class Fixture {
public:
  Fixture(const bool counted_loop, const bool external, const bool dynamic)
      : counted(counted_loop),
        code(counted ? std::vector<Byte>(counted_code.begin(), counted_code.end())
                     : std::vector<Byte>(conditional_code.begin(), conditional_code.end())) {
    std::ranges::copy(code, ram.begin() + entry);
    const auto end = static_cast<Address>(entry + code.size());
    regions = {
        Ram{.bytes = std::span(ram).first(entry)},
        Ram{.first = entry, .bytes = std::span(ram).subspan(entry, code.size()), .writable = false},
        Ram{.first = end, .bytes = std::span(ram).subspan(end)}};
    const auto memory = external ? Memory::window(regions[1]) : Memory::mapped(regions);
    const std::array spans{CodeSpan{.address = entry, .words = code}};
    constexpr std::array entries{entry};
    const std::array variants{CodeVariant{.id = 0, .code = spans, .entries = entries}};
    std::vector<RegionLayout> layout;
    for (const auto &region : memory.regions()) {
      layout.push_back(
          {.first = region.first, .size = region.bytes.size(), .writable = region.writable});
    }
    const std::array layouts{VariantLayout{.id = 0, .memory = layout}};
    const auto image =
        Core::compile({.variants = variants},
                      {.memory = dynamic ? std::span<const VariantLayout>{} : layouts,
                       .interrupts = {},
                       .transitions = {},
                       .callbacks = {},
                       .bindings = {},
                       .topology = dynamic ? MemoryTopology::dynamic : MemoryTopology::fixed});
    core.set_memory(memory);
    core.install(image, 0);
  }

  Prediction reset(const Byte ccr) {
    ram.fill(0);
    std::ranges::copy(code, ram.begin() + entry);
    core.set_registers({.pc = entry,
                        .d = counted ? counted_initial : conditional_initial,
                        .x = data,
                        .y = 7,
                        .sp = stack,
                        .ccr = ccr});
    accesses = 0;
    observed = true;
    origin = core.progress().estimated_cycles;
    return {.registers = core.registers(), .ram = ram};
  }

  static bool write(void *owner, const WordWrite request) noexcept {
    auto &f = *static_cast<Fixture *>(owner);
    ++f.accesses;
    const auto &r = f.core.registers();
    const auto value = static_cast<Address>((f.counted ? counted_initial : conditional_initial) +
                                            (3 * f.accesses));
    const auto address = static_cast<Address>(data + (2 * (f.accesses - 1)));
    f.observed &= r.pc == entry + 3 && r.d == value && r.x == address &&
                  r.y == (f.counted ? 8 - f.accesses : 7) &&
                  f.core.progress().estimated_cycles ==
                      f.origin + (std::uint64_t{7} * (f.accesses - 1)) + 2 &&
                  request.address == address && request.value == value;
    if (f.accesses == f.refuse) { return false; }
    f.ram[request.address] = static_cast<Byte>(request.value >> byte_bits);
    f.ram[static_cast<Address>(request.address + 1)] = static_cast<Byte>(request.value);
    if (f.accesses == f.yield) { f.core.request_yield(); }
    return true;
  }

  bool counted, observed = true;
  std::vector<Byte> code;
  std::array<Byte, address_bytes> ram{};
  Io io{.owner = this, .write_word = write};
  Core core{Memory::external(), &io};
  std::array<Ram, 3> regions;
  unsigned accesses = 0, refuse = 0, yield = 0;
  std::uint64_t origin = 0;
};

void prefixes(Fixture &f) {
  for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
    for (unsigned count = 0; count <= steps; ++count) {
      auto wanted = f.reset(static_cast<Byte>(ccr));
      for (unsigned i = 0; i < count; ++i) { wanted.cycles += step(wanted, f.counted); }
      const auto result = f.core.run(count);
      require(result.fault == Fault::none && result.retired == count && f.observed &&
                  f.core.registers() == wanted.registers && f.ram == wanted.ram &&
                  f.core.progress().estimated_cycles == f.origin + wanted.cycles,
              "Loop instruction grant changed its exact prefix or observed progress");
    }
  }
  for (unsigned deadline = 0; deadline <= deadline_bound; ++deadline) {
    auto wanted = f.reset(initial_flags);
    unsigned count = 0;
    while (wanted.cycles < deadline) {
      wanted.cycles += step(wanted, f.counted);
      ++count;
    }
    const auto result = f.core.run_until(f.origin + deadline);
    require(result.fault == Fault::none && result.retired == count && f.observed &&
                f.core.registers() == wanted.registers && f.ram == wanted.ram &&
                f.core.progress().estimated_cycles == f.origin + wanted.cycles,
            "Loop cycle grant changed its exact atomic prefix");
  }
}

void exits(Fixture &f) {
  for (unsigned access = 1; access <= (f.counted ? 7U : 5U); ++access) {
    for (const bool failure : {false, true}) {
      auto wanted = f.reset(initial_flags);
      f.refuse = failure ? access : 0;
      f.yield = failure ? 0 : access;
      const unsigned count = (3 * (access - 1)) + (failure ? 1U : 2U);
      for (unsigned i = 0; i < count; ++i) { wanted.cycles += step(wanted, f.counted); }
      const auto result = f.core.run(steps);
      require(result.retired == count &&
                  result.fault == (failure ? Fault::memory_write : Fault::none) &&
                  f.accesses == access && f.observed && f.core.registers() == wanted.registers &&
                  f.ram == wanted.ram &&
                  f.core.progress().estimated_cycles == f.origin + wanted.cycles,
              "A loop fault or yield lost completed turns, memory, or the current prefix");
    }
  }
}

void corners(Fixture &f) {
  constexpr std::array<Address, 3> counters{0, 1, 0xffff};
  constexpr std::array<Address, 3> offsets{0, 3, 5};
  for (const Address counter : counters) {
    for (const Address offset : offsets) {
      for (unsigned bound = 0; bound <= deadline_bound; ++bound) {
        auto wanted = f.reset(initial_flags);
        wanted.registers.pc += offset;
        wanted.registers.y = counter;
        f.core.set_registers(wanted.registers);
        unsigned count = 0;
        while (wanted.cycles < bound) {
          wanted.cycles += step(wanted, f.counted);
          ++count;
        }
        const auto result = f.core.run_until(f.origin + bound);
        require(result.fault == Fault::none && result.retired == count &&
                    f.core.registers() == wanted.registers && f.ram == wanted.ram &&
                    f.core.progress().estimated_cycles == f.origin + wanted.cycles,
                "A wrapped loop counter or interior entry changed the cycle prefix");
      }
    }
  }
}

void overlapping_entry() {
  constexpr Byte decrement_y = 0x36;
  constexpr Byte back_to_nop = static_cast<Byte>(-5);
  constexpr Byte back_to_branch = static_cast<Byte>(-2);
  // BRN skips its operand, which is also an independently reachable NOP.
  std::array code{isa::opcode<isa::Opcode::brn_relative>()[0],
                  isa::nop[0],
                  isa::incb[0],
                  isa::opcode<isa::Opcode::dbne_relative>()[0],
                  decrement_y,
                  back_to_nop,
                  isa::opcode<isa::Opcode::bra_relative>()[0],
                  back_to_branch};
  constexpr Address loop = entry + 1;
  constexpr Address done = entry + 6;
  constexpr unsigned turns = 3;
  constexpr unsigned instructions_per_turn = 3;
  constexpr std::uint64_t cycles_per_turn = 5;
  constexpr std::array entries{entry, loop};
  const std::array spans{CodeSpan{.address = entry, .words = std::span(code).first(2)},
                         CodeSpan{.address = loop, .words = std::span(code).subspan(1)}};
  const std::array variants{CodeVariant{.id = 0, .code = spans, .entries = entries}};
  const std::array layout{RegionLayout{.first = entry, .size = code.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = 0, .memory = layout}};
  const auto image = Core::compile(
      {.variants = variants},
      {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
  Core core(Memory::window({.first = entry, .bytes = code, .writable = false}));
  core.install(image, 0);
  for (const auto start : entries) {
    core.set_registers({.pc = start, .y = turns, .ccr = initial_flags});
    auto expected = core.registers();
    expected.pc = done;
    expected.y = 0;
    expected.d = turns;
    const auto prior = core.progress().estimated_cycles;
    const auto result = core.run(turns * instructions_per_turn);
    require(result.fault == Fault::none && result.retired == turns * instructions_per_turn &&
                core.registers() == expected &&
                core.progress().estimated_cycles == prior + (turns * cycles_per_turn),
            "The overlapping fallthrough and loop head reach the same exact final state");
  }
}
}

int main() {
  try {
    overlapping_entry();
    for (const bool counted : {false, true}) {
      for (const bool dynamic : {false, true}) {
        Fixture mapped(counted, false, dynamic);
        prefixes(mapped);
        corners(mapped);
        Fixture external(counted, true, dynamic);
        prefixes(external);
        exits(external);
      }
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
