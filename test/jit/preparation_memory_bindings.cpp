// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "preparation_memory.hpp"

#include <array>
#include <span>
#include <stdexcept>

namespace cpu12::test {
namespace {
constexpr Address entry = 0x4000, data_address = 0x2000, wrapped_address = 0xffff;
constexpr unsigned byte_bits = 8;
constexpr Byte byte_mask = 0xff, negative = 8;
constexpr Byte load_backedge = 0xfb, original_high = 0x12, original_low = 0x34;
constexpr std::array entries{entry};
constexpr auto code = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                                   std::array<Byte, 2>{0x20, 0},
                                   isa::opcode<isa::Opcode::std_extended>(),
                                   std::array<Byte, 2>{0x20, 2},
                                   isa::opcode<isa::Opcode::bra_relative>(),
                                   std::array<Byte, 1>{0xf8});

void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

struct Storage {
  std::array<Byte, 4> bytes{};
  std::array<Byte, 4> known{1, 1, 0, 0};

  auto regions(std::span<Byte> program) {
    return std::array{Ram{.first = entry, .bytes = program, .writable = false},
                      Ram{.first = data_address, .bytes = bytes, .known = known}};
  }
};

void check(Core &core, Storage &storage, const Address value) {
  storage.bytes = {static_cast<Byte>(value >> byte_bits), static_cast<Byte>(value), 0, 0};
  storage.known = {1, 1, 0, 0};
  const Registers seed{
      .pc = entry, .d = 0xffff, .x = 0x1234, .y = 0x5678, .sp = 0x9000, .ccr = byte_mask};
  core.set_registers(seed);
  const auto progress = core.progress();
  auto wanted = seed;
  wanted.d = value;
  wanted.pc = entry + 6;
  constexpr Byte retained = 0xf1;
  wanted.ccr = static_cast<Byte>(retained | (value >= 0x8000 ? negative : 0));
  require(
      core.run(2).retired == 2 && core.state() == State{.registers = wanted, .failure = {}} &&
          core.progress().estimated_cycles == progress.estimated_cycles + 6 &&
          storage.bytes ==
              std::array{storage.bytes[0], storage.bytes[1], storage.bytes[0], storage.bytes[1]} &&
          storage.known == std::array<Byte, 4>{1, 1, 1, 1},
      "Rebound LLVM loads/stores use current borrowed RAM and initialize exact knownness");
}

void rebinding() {
  auto program = code;
  Storage first;
  Storage second;
  auto first_regions = first.regions(program);
  auto second_regions = second.regions(program);
  Core core(Memory::mapped(first_regions));
  require(core.prepare(entries).prepared_entries == 3, "Mapped loop must compile completely");
  const auto compilations = core.statistics().compiled_entries;
  constexpr Address first_value = 0x1234;
  constexpr Address second_value = 0x80ff;
  check(core, first, first_value);
  const auto first_bytes = first.bytes;
  core.set_memory(Memory::mapped(second_regions));
  require(core.prepare(entries).native_bytes == 0 &&
              core.statistics().compiled_entries == compilations,
          "Identical code/topology reuses LLVM code across different concrete RAM bindings");
  check(core, second, second_value);
  require(first.bytes == first_bytes, "Rebound code never touches the previous RAM owner");
  const std::array reordered{second_regions[1], second_regions[0]};
  core.set_memory(Memory::mapped(reordered));
  require(core.prepare(entries).native_bytes != 0,
          "Changed binding topology requires matching lowering");
  check(core, second, second_value);
  core.set_memory(Memory::mapped(first_regions));
  require(core.prepare(entries).native_bytes == 0,
          "Returning to a retained binding reuses its own native code");
  check(core, first, first_value);
  require(program == code, "Rebinding never alters immutable executable input");
}

struct Port {
  unsigned calls = 0;
  static constexpr Address value = 0xabcd;

  static bool read_word(void *owner, const Address address, Address &output) noexcept {
    ++static_cast<Port *>(owner)->calls;
    output = value;
    return address == data_address || address == wrapped_address;
  }
};

void knownness() {
  auto program = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                              std::array<Byte, 2>{0x20, 0},
                              isa::opcode<isa::Opcode::bra_relative>(),
                              std::array<Byte, 1>{load_backedge});
  std::array<Byte, 2> bytes{original_high, original_low};
  std::array<Byte, 2> known{};
  const std::array regions{Ram{.first = entry, .bytes = program, .writable = false},
                           Ram{.first = data_address, .bytes = bytes, .known = known}};
  Port port;
  Io io{.owner = &port, .read_word = Port::read_word};
  Core core(Memory::mapped(regions), &io);
  require(core.prepare(entries).prepared_entries == 2, "Tracked word read must prepare completely");
  const auto compilations = core.statistics().compiled_entries;
  for (unsigned mask = 0; mask < 4; ++mask) {
    known = {static_cast<Byte>(mask & 1U), static_cast<Byte>((mask >> 1U) & 1U)};
    core.set_registers({.pc = entry});
    const auto calls = port.calls;
    constexpr Address ram_value = 0x1234;
    require(core.run(1).retired == 1 &&
                core.registers().d == (mask == 3 ? ram_value : Port::value) &&
                port.calls == calls + (mask == 3 ? 0U : 1U),
            "A word reads mapped bytes only when both are known; every other case is one real word "
            "transaction");
  }
  require(core.statistics().compiled_entries == compilations,
          "Live knownness never triggers translation");
}

void split_word() {
  auto program = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                              std::array<Byte, 2>{byte_mask, byte_mask},
                              isa::opcode<isa::Opcode::bra_relative>(),
                              std::array<Byte, 1>{load_backedge});
  std::array<Byte, 1> high{original_high};
  std::array<Byte, 1> low{original_low};
  const std::array regions{Ram{.first = entry, .bytes = program, .writable = false},
                           Ram{.first = wrapped_address, .bytes = high},
                           Ram{.bytes = low}};
  Port port;
  Io io{.owner = &port, .read_word = Port::read_word};
  Core core(Memory::mapped(regions), &io);
  require(core.prepare(entries).prepared_entries == 2,
          "Wrapped mapped read prepares native semantics");
  core.set_registers({.pc = entry});
  require(
      core.run(1).retired == 1 && core.registers().d == Port::value && port.calls == 1 &&
          high[0] != static_cast<Byte>(Port::value >> byte_bits),
      "A word spanning separate grants remains one bus transaction, with no invented split access");
}
}

void prepared_memory_bindings() {
  rebinding();
  knownness();
  split_word();
}
}
