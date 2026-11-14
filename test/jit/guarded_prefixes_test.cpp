// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address entry = 0x4000, data_first = 0x5000;
constexpr std::uint64_t variant = 7, infinite = std::numeric_limits<std::uint64_t>::max();
constexpr Address payload = 0x5aa5;
constexpr unsigned byte_values = 256;
constexpr std::size_t data_bytes = 64;
constexpr Byte initial_flags = 0x21, loaded_flags = initial_flags | 8;
constexpr Byte payload_a = payload >> 8U, payload_b = static_cast<Byte>(payload);

void require(const bool passed, const char *message) {
  if (!passed) { throw std::runtime_error(message); }
}

struct Bus {
  unsigned calls = 0;

  template <class Value>
  static bool
  read(void *owner, [[maybe_unused]] Address address, [[maybe_unused]] Value &value) noexcept {
    ++static_cast<Bus *>(owner)->calls;
    return false;
  }

  template <class Access> static bool write(void *owner, [[maybe_unused]] Access access) noexcept {
    ++static_cast<Bus *>(owner)->calls;
    return false;
  }

  Io callbacks() noexcept {
    return {.owner = this,
            .read = read<Byte>,
            .write = write<ByteWrite>,
            .read_word = read<Address>,
            .write_word = write<WordWrite>};
  }
};

std::vector<Byte> loop(const std::span<const Byte> prefix) {
  std::vector<Byte> bytes(prefix.begin(), prefix.end());
  bytes.reserve(prefix.size() + 2);
  bytes.push_back(isa::opcode<isa::Opcode::bra_relative>()[0]);
  bytes.push_back(static_cast<Byte>(byte_values - prefix.size() - 2));
  return bytes;
}

NativeImage compile(const std::span<Byte> code, const std::span<const Ram> memory) {
  const std::array spans{CodeSpan{.address = entry, .words = code}};
  const std::array roots{entry};
  const std::array variants{CodeVariant{.id = variant, .code = spans, .entries = roots}};
  std::vector<RegionLayout> grants;
  grants.reserve(memory.size());
  for (const auto &region : memory) {
    grants.push_back({.first = region.first,
                      .size = region.bytes.size(),
                      .writable = region.writable,
                      .tracked = !region.known.empty()});
  }
  const std::array layouts{VariantLayout{.id = variant, .memory = grants}};
  return Core::compile(
      {.variants = variants},
      {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
}

struct Fixture {
  std::vector<Byte> code;
  std::array<Byte, data_bytes> data{};
  std::array<Byte, data_bytes> known{};
  std::array<Ram, 2> regions;
  Bus bus;
  Io io = bus.callbacks();
  Core core;

  explicit Fixture(const std::span<const Byte> prefix, const bool writable = true)
      : code(loop(prefix)),
        regions{Ram{.first = entry, .bytes = code, .writable = false},
                Ram{.first = data_first, .bytes = data, .known = known, .writable = writable}},
        core(Memory::mapped(regions), &io) {
    data.fill(static_cast<Byte>(payload));
    known.fill(1);
    known[4] = 0;
    core.install(compile(code, regions), variant);
  }
};

void all_indices(const std::span<const Byte> operation, const bool writable) {
  Fixture fixture(operation, writable);
  for (unsigned base = 0; base < address_bytes; ++base) {
    const auto pointer = static_cast<Address>(base);
    fixture.core.set_registers({.pc = entry,
                                .d = payload,
                                .x = pointer,
                                .y = pointer,
                                .sp = pointer,
                                .ccr = initial_flags});
    const auto before = fixture.core.state();
    const auto time = fixture.core.progress();
    const auto memory = fixture.data;
    const auto known = fixture.known;
    const auto calls = fixture.bus.calls;
    const auto prefix = fixture.core.foreground_prefix();
    require((prefix == 0 || prefix == infinite) && fixture.core.event_prefix() == prefix &&
                fixture.core.state() == before && fixture.core.progress() == time &&
                fixture.data == memory && fixture.known == known && fixture.bus.calls == calls,
            "Loop admission inspects all stable index values without guest or owner effects");
    const auto executed = fixture.core.run(1);
    const bool direct = executed.retired == 1 && executed.fault == Fault::none;
    require((prefix == infinite) == direct && (fixture.bus.calls == calls) == direct,
            "Every guarded read/write footprint agrees with actual generated LLVM access");
  }
}

void footprints() {
  const auto byte_store =
      isa::program(isa::opcode<isa::Opcode::staa_indexed>(), std::array<Byte, 1>{0});
  const auto word_store =
      isa::program(isa::opcode<isa::Opcode::std_indexed>(), std::array<Byte, 1>{0x5f});
  const auto byte_load =
      isa::program(isa::opcode<isa::Opcode::ldaa_indexed>(), std::array<Byte, 1>{0x81});
  const auto word_load =
      isa::program(isa::opcode<isa::Opcode::ldd_indexed>(), std::array<Byte, 3>{0xe2, 0xff, 0xff});
  const auto increment =
      isa::program(isa::opcode<isa::Opcode::inc_indexed>(), std::array<Byte, 1>{0});
  const auto clear = isa::program(isa::opcode<isa::Opcode::clr_indexed>(), std::array<Byte, 1>{0});
  for (const bool writable : {false, true}) {
    all_indices(byte_store, writable);
    all_indices(word_store, writable);
    all_indices(byte_load, writable);
    all_indices(word_load, writable);
    all_indices(increment, writable);
    all_indices(clear, writable);
  }
}

void persistent_loop() {
  const auto body = isa::program(isa::opcode<isa::Opcode::staa_indexed>(),
                                 std::array<Byte, 1>{0},
                                 isa::opcode<isa::Opcode::ldab_indexed>(),
                                 std::array<Byte, 1>{0x40});
  Fixture fixture(body);
  fixture.core.set_registers(
      {.pc = entry, .d = payload, .x = data_first, .y = data_first + 1, .ccr = initial_flags});
  require(fixture.core.foreground_prefix() == infinite,
          "One admission covers both invariant pointers throughout the whole memory loop");
  constexpr unsigned iterations = 4096;
  constexpr std::uint64_t period = 8;
  const auto result = fixture.core.run_until(iterations * period);
  require(result.fault == Fault::none && result.retired == iterations * 3 &&
              fixture.core.progress().estimated_cycles == iterations * period &&
              fixture.core.registers() == Registers{.pc = entry,
                                                    .d = payload,
                                                    .x = data_first,
                                                    .y = data_first + 1,
                                                    .ccr = loaded_flags} &&
              fixture.data[0] == payload_a && fixture.data[1] == payload_b &&
              fixture.bus.calls == 0,
          "Thousands of native loop iterations preserve independently counted time and state");
  auto next = fixture.core.registers();
  next.y = data_first - 1;
  fixture.core.set_registers(next);
  require(fixture.core.foreground_prefix() == 0,
          "A later access through the other pointer revokes whole-loop admission at entry");
}

void closing_invariant() {
  const auto body = isa::program(isa::opcode<isa::Opcode::staa_indexed>(),
                                 std::array<Byte, 1>{0},
                                 isa::opcode<isa::Opcode::inx>());
  Fixture fixture(body);
  fixture.core.set_registers(
      {.pc = entry, .d = payload, .x = static_cast<Address>(data_first + fixture.data.size() - 1)});
  require(fixture.core.foreground_prefix() == 2 && fixture.core.run_until(2).retired == 1,
          "A pointer mutation ends the memory guard before it can certify a later iteration");
  require(fixture.core.foreground_prefix() == 4 && fixture.core.run_until(6).retired == 2 &&
              fixture.core.registers().pc == entry && fixture.core.foreground_prefix() == 0,
          "The real updated pointer is checked again before the now-unmapped next store");
}

void wrapping_rom() {
  std::array<Byte, address_bytes> rom{};
  const auto body =
      loop(isa::program(isa::opcode<isa::Opcode::ldd_indexed>(), std::array<Byte, 1>{0}));
  std::ranges::copy(body, rom.begin() + entry);
  rom.back() = payload_a;
  rom.front() = payload_b;
  const std::array regions{Ram{.bytes = rom, .known = std::span(rom).first(0), .writable = false}};
  Core core(Memory::mapped(regions));
  core.install(compile(std::span(rom).subspan(entry, body.size()), regions), variant);
  core.set_registers({.pc = entry, .x = std::numeric_limits<Address>::max()});
  require(core.foreground_prefix() == infinite && core.run(1).retired == 1 &&
              core.registers().d == payload,
          "A word wraps within the same full grant and an empty knowledge span remains empty");
}

void knowledge_alias() {
  auto code = loop(isa::program(isa::opcode<isa::Opcode::staa_indexed>(),
                                std::array<Byte, 1>{2},
                                isa::opcode<isa::Opcode::ldab_indexed>(),
                                std::array<Byte, 1>{1}));
  std::array<Byte, data_bytes + 1> storage{};
  storage.fill(1);
  const std::array regions{Ram{.first = entry, .bytes = code, .writable = false},
                           Ram{.first = data_first,
                               .bytes = std::span(storage).first(data_bytes),
                               .known = std::span(storage).last(data_bytes)}};
  Bus bus;
  auto io = bus.callbacks();
  Core core(Memory::mapped(regions), &io);
  core.install(compile(code, regions), variant);
  core.set_registers({.pc = entry, .x = data_first});
  require(core.foreground_prefix() == 0,
          "An overlapping byte/knowledge plane cannot promise monotonic read initialization");
  const auto result = core.run(16);
  require(result.retired == 1 && result.fault == Fault::memory_read &&
              result.address == data_first + 1 && core.progress().estimated_cycles == 2 &&
              bus.calls == 1,
          "The real store clears the next read's knowledge bit through that physical alias");
}

void fault_boundary() {
  const auto body = isa::program(isa::opcode<isa::Opcode::staa_indexed>(),
                                 std::array<Byte, 1>{0},
                                 isa::opcode<isa::Opcode::daa>());
  Fixture fixture(body);
  constexpr Address invalid_decimal = 0x955a;
  constexpr Byte half = 0x20;
  fixture.core.set_registers({.pc = entry, .d = invalid_decimal, .x = data_first, .ccr = half});
  require(fixture.core.foreground_prefix() == 2 && fixture.core.run_until(2).retired == 1 &&
              fixture.core.foreground_prefix() == 0,
          "A possible native arithmetic fault ends admission before separate input validation");
  const auto result = fixture.core.run(16);
  require(result.fault == Fault::unsupported_state && result.retired == 0 &&
              fixture.core.progress().estimated_cycles == 2 && fixture.bus.calls == 0,
          "The actual invalid decimal state faults at that boundary without owner effects");
}
}

int main() try {
  footprints();
  persistent_loop();
  closing_invariant();
  wrapping_rom();
  knowledge_alias();
  fault_boundary();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
