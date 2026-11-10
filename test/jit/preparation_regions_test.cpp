// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address first = 0x4000, stride = 16, stack = 0x9000;
constexpr unsigned byte_bits = 8, sign = 0x8000;
constexpr Byte preserved = 0xf0, load_preserved = 0xf1;
constexpr unsigned carry = 1, overflow = 2, zero = 4, negative = 8;
constexpr std::size_t component_count = 512;
constexpr Byte backedge = 0xfb;

void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

struct Fixture {
  Fixture() : bytes(component_count * stride), entries(component_count) {
    for (std::size_t index = 0; index < entries.size(); ++index) {
      entries[index] = static_cast<Address>(first + (index * stride));
      const auto value = static_cast<Address>(index);
      const std::array code{static_cast<Byte>(isa::Opcode::ldd_immediate),
                            static_cast<Byte>(value >> byte_bits),
                            static_cast<Byte>(value),
                            static_cast<Byte>(isa::Opcode::addd_immediate),
                            Byte{0},
                            Byte{1},
                            static_cast<Byte>(isa::Opcode::bra_relative),
                            backedge};
      for (std::size_t offset = 0; offset < code.size(); ++offset) {
        bytes[(index * stride) + offset] = code[offset];
      }
    }
  }

  std::vector<Byte> bytes;
  std::vector<Address> entries;
};

std::uint64_t step(Registers &state, const Address base) {
  if (state.pc == base) {
    state.d = static_cast<Address>((base - first) / stride);
    state.ccr = static_cast<Byte>((state.ccr & load_preserved) | (state.d == 0 ? zero : 0) |
                                  ((state.d & sign) != 0 ? negative : 0));
    state.pc = static_cast<Address>(base + 3);
    return 2;
  }
  if (state.pc == base + 3) {
    const auto old = state.d;
    state.d = static_cast<Address>(old + 1);
    state.ccr = static_cast<Byte>((state.ccr & preserved) |
                                  (old == std::numeric_limits<Address>::max() ? carry : 0) |
                                  (old == sign - 1 ? overflow : 0) | (state.d == 0 ? zero : 0) |
                                  ((state.d & sign) != 0 ? negative : 0));
    state.pc = static_cast<Address>(base + 6);
    return 2;
  }
  state.pc = static_cast<Address>(base + 3);
  return 3;
}

void check(Core &core, const Address base) {
  constexpr std::array<Address, 4> operands{0, 0x7fff, 0x8000, 0xffff};
  constexpr std::array<Byte, 4> flags{0, 0x55, 0xaa, 0xff};
  for (const auto operand : operands) {
    for (const auto ccr : flags) {
      for (const Address offset : {Address{0}, Address{3}, Address{6}}) {
        const Registers initial{.pc = static_cast<Address>(base + offset),
                                .d = operand,
                                .x = base,
                                .y = static_cast<Address>(~base),
                                .sp = stack,
                                .ccr = ccr};
        for (unsigned grant = 0; grant <= 7; ++grant) {
          core.set_registers(initial);
          auto expected = initial;
          std::uint64_t cycles = 0;
          for (unsigned index = 0; index < grant; ++index) { cycles += step(expected, base); }
          const auto before = core.progress();
          const auto result = core.run(grant);
          require(
              result.fault == Fault::none && result.retired == grant &&
                  core.registers() == expected &&
                  core.progress() == Progress{.estimated_cycles = before.estimated_cycles + cycles},
              "Every disconnected region entry preserves independent register/flag/prefix results");
        }
      }
    }
  }
}

void independent_regions() {
  Fixture fixture;
  const auto source = fixture.bytes;
  Core core(Memory::window({.first = first, .bytes = fixture.bytes, .writable = false}));
  const auto state = core.state();
  const auto progress = core.progress();
  const auto started = std::chrono::steady_clock::now();
  const auto report = core.prepare(fixture.entries);
  const auto seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  require(report.prepared_entries == component_count * 3 && report.unresolved_entries == 0 &&
              report.native_bytes > 0 && report.failure.fault == Fault::none &&
              core.state() == state && core.progress() == progress,
          "All disconnected executable regions prepare without architecture or coverage loss");
  const auto statistics = core.statistics();
  for (const auto entry : fixture.entries) { check(core, entry); }
  const auto reused = core.prepare(fixture.entries);
  require(reused.native_bytes == 0 && reused.prepared_entries == report.prepared_entries &&
              core.statistics().compiled_entries == statistics.compiled_entries &&
              fixture.bytes == source,
          "Entry-specific native routing reuses one retained image without runtime recompilation");
  std::cout << "components=" << component_count << " prepared=" << report.prepared_entries
            << " cold-seconds=" << seconds << " native-bytes=" << report.native_bytes << '\n';
}

void incremental_regions() {
  Fixture fixture;
  const auto memory = Memory::window({.first = first, .bytes = fixture.bytes, .writable = false});
  Core growing(memory);
  Core fresh(memory);
  const std::array initial{first};
  const std::array expanded{first, static_cast<Address>(first + stride)};
  const std::array added{expanded.back()};
  const auto first_image = growing.prepare(initial);
  const auto next_image = growing.prepare(expanded);
  const auto isolated = fresh.prepare(added);
  require(first_image.prepared_entries == 3 && next_image.prepared_entries == 6 &&
              isolated.prepared_entries == 3 && next_image.native_bytes == isolated.native_bytes &&
              next_image.native_bytes > 0,
          "Extending prepared reachability emits only new regions, retaining old immutable code");
  check(growing, first);
  check(growing, added.front());
}

void rebinding_regions() {
  Fixture fixture;
  auto changed = fixture.bytes;
  changed[5] = 2; // Keep the entry instruction; change its later ADDD operand.
  constexpr Address ram_first = 0x2000;
  std::array<Byte, 32> first_ram{};
  std::array<Byte, 64> second_ram{};
  const std::array<Ram, 2> original{
      Ram{.first = ram_first, .bytes = first_ram},
      Ram{.first = first, .bytes = fixture.bytes, .writable = false, .identity = 1}};
  const std::array<Ram, 2> rebound{
      Ram{.first = ram_first, .bytes = second_ram},
      Ram{.first = first, .bytes = fixture.bytes, .writable = false, .identity = 2}};
  const std::array<Ram, 2> updated{
      Ram{.first = ram_first, .bytes = second_ram},
      Ram{.first = first, .bytes = changed, .writable = false, .identity = 3}};
  const std::array entries{first, static_cast<Address>(first + stride)};
  Core core(Memory::mapped(original));
  require(core.prepare(entries).prepared_entries == 6, "Prepare both initial code regions");
  const auto state = core.state();
  const auto count = core.statistics().compiled_entries;
  core.set_memory(Memory::mapped(rebound));
  const auto reused = core.prepare(entries);
  require(reused.prepared_entries == 6 && reused.native_bytes == 0 && core.state() == state &&
              core.statistics().compiled_entries == count,
          "Unchanged immutable region bytes reuse native code across unrelated RAM bindings");
  check(core, first);
  check(core, entries.back());
  core.set_memory(Memory::mapped(updated));
  const auto replacement = core.prepare(entries);
  Core fresh(Memory::window({.first = first, .bytes = changed, .writable = false}));
  const std::array changed_entry{first};
  const auto isolated = fresh.prepare(changed_entry);
  require(
      replacement.prepared_entries == 6 && replacement.native_bytes > 0 &&
          replacement.native_bytes == isolated.native_bytes,
      "A changed interior instruction invalidates its complete region and retains unrelated code");
  core.set_registers({.pc = first});
  const auto before = core.progress();
  const auto result = core.run(2);
  require(result.retired == 2 && result.fault == Fault::none &&
              core.registers() == Registers{.pc = static_cast<Address>(first + 6), .d = 2} &&
              core.progress() == Progress{.estimated_cycles = before.estimated_cycles + 4},
          "Code with the same entry bytes and changed later bytes executes its new semantics");
  core.set_memory(Memory::mapped(original));
  require(core.prepare(entries).native_bytes == 0, "Original immutable code remains retained");
  check(core, first);
}
}

int main() try {
  independent_regions();
  incremental_regions();
  rebinding_regions();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
