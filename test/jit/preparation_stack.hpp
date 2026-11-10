// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpu12::test::prepared_stack {
constexpr Address entry = 0x4000;
constexpr unsigned byte_bits = 8, byte_mask = 0xff;
constexpr Byte bra = 0x20, backedge = 0xfe, padding = 0x18;

inline void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

struct Program {
  std::vector<Byte> code;
  std::vector<Address> entries;
  std::size_t nodes = 0, unresolved = 0;
};

struct Fixture {
  explicit Fixture(const Program &program) : end(entry + program.code.size()) {
    for (std::size_t address = 0; address < bytes.size(); ++address) {
      bytes[address] = static_cast<Byte>((address * 13U) ^ (address >> byte_bits));
    }
    std::ranges::copy(program.code, bytes.begin() + entry);
    grants = {Ram{.bytes = std::span(bytes).first(entry)},
              Ram{.first = entry,
                  .bytes = std::span(bytes).subspan(entry, program.code.size()),
                  .writable = false},
              Ram{.first = static_cast<Address>(end), .bytes = std::span(bytes).subspan(end)}};
  }

  [[nodiscard]] bool writable(const Address address) const {
    return address < entry || address >= end;
  }

  std::array<Byte, address_bytes> bytes{};
  std::size_t end;
  std::array<Ram, 3> grants{};
};

struct Bus {
  Fixture &fixture;
  Core *core = nullptr;
  State expected{};
  Progress progress{};
  unsigned calls = 0;
  bool observed = true;

  void observe() noexcept {
    ++calls;
    observed = observed && core->state() == expected && core->progress() == progress;
  }

  static bool read_word(void *owner, const Address address, Address &value) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    self.observe();
    value = static_cast<Address>((static_cast<unsigned>(self.fixture.bytes[address]) << byte_bits) |
                                 self.fixture.bytes[static_cast<Address>(address + 1U)]);
    return true;
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    self.observe();
    const auto next = static_cast<Address>(access.address + 1U);
    if (!self.fixture.writable(access.address) || !self.fixture.writable(next)) { return false; }
    self.fixture.bytes[access.address] = static_cast<Byte>(access.value >> byte_bits);
    self.fixture.bytes[next] = static_cast<Byte>(access.value);
    return true;
  }

  [[nodiscard]] Io io() {
    return {.owner = this, .read_word = read_word, .write_word = write_word};
  }
};

struct Pair {
  explicit Pair(const Program &program)
      : optimized(program),
        reference(program),
        optimized_bus{.fixture = optimized},
        reference_bus{.fixture = reference},
        optimized_io(optimized_bus.io()),
        reference_io(reference_bus.io()),
        prepared(Memory::mapped(optimized.grants), &optimized_io),
        native(Memory::mapped(reference.grants), &reference_io) {
    optimized_bus.core = &prepared;
    reference_bus.core = &native;
    const auto state = prepared.state();
    const auto report = prepared.prepare(program.entries);
    require(report.failure.fault == Fault::none && report.prepared_entries == program.nodes &&
                report.unresolved_entries == program.unresolved && report.native_bytes != 0 &&
                prepared.state() == state && prepared.progress() == Progress{} &&
                optimized.bytes == reference.bytes,
            "Stack/control preparation must lower all declared nodes without execution or I/O");
    compilations = prepared.statistics().compiled_entries;
  }

  void seed(const Registers before) {
    prepared.set_registers(before);
    native.set_registers(before);
    optimized_bus.expected = prepared.state();
    reference_bus.expected = native.state();
    optimized_bus.progress = prepared.progress();
    reference_bus.progress = native.progress();
    optimized_bus.calls = reference_bus.calls = 0;
  }

  void retained() const {
    require(prepared.statistics().compiled_entries == compilations && optimized_bus.observed &&
                reference_bus.observed,
            "Prepared stack/control must retain native code and publish exact pre-I/O state");
  }

  Fixture optimized, reference;
  Bus optimized_bus, reference_bus;
  Io optimized_io, reference_io;
  Core prepared, native;
  std::uint64_t compilations = 0;
};

std::size_t calls();
std::size_t fixed_calls();
void boundaries();
}
