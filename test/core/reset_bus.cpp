// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "observed_core.hpp"
#include "reset_model.hpp"

#include <array>

namespace cpu12::test::resetting {
namespace {
struct Bus {
  ObservedCore *core = nullptr;
  static constexpr Address initial_target = 0x1234;
  Address target = initial_target, observed = 0;
  Address vector_address = vector;
  unsigned words = 0, bytes = 0;
  bool allow = false, probe_reentry = false, unchanged = true;

  static bool word(void *owner, const Address address, Address &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.words;
    bus.observed = address;
    bus.unchanged &= bus.core->registers() == seed;
    if (bus.probe_reentry) {
      const auto result = bus.core->reset(vector);
      bus.unchanged &= !result.accepted && result.fault == Fault::reentrant &&
                       bus.core->run(0).fault == Fault::reentrant;
    }
    value = bus.target;
    return bus.allow && address == bus.vector_address;
  }

  static bool byte(void *owner, Address /*address*/, Byte &value) noexcept {
    ++static_cast<Bus *>(owner)->bytes;
    value = 0;
    return false;
  }
};

void failure_and_recovery() {
  Bus bus;
  Io io{.owner = &bus, .read_word = Bus::word};
  ObservedCore core(Memory::external(), &io);
  bus.core = &core;
  core.set_registers(seed);
  const auto result = core.reset(vector);
  require(!result.accepted && result.fault == Fault::memory_read && result.pc == seed.pc &&
              result.address == vector && result.value == bus.target && result.access_size == 2 &&
              core.registers() == seed && bus.words == 1 && bus.unchanged,
          "A refused vector read leaves CPU state unchanged and reports the real access");
  require(core.step().fault == Fault::memory_read && bus.words == 1,
          "A failed reset latches without retrying its vector through instruction execution");
  bus.allow = true;
  bus.probe_reentry = true;
  accept(core, vector, seed, bus.target);
  require(bus.words == 2 && bus.unchanged && core.statistics().native_instructions == 0 &&
              core.statistics().read_callbacks == 2,
          "Explicit reset retries once after recovery and rejects recursive reset/execution");
}

void prior_data_failure() {
  Bus bus{.allow = true};
  Io io{.owner = &bus, .read = Bus::byte, .read_word = Bus::word};
  ObservedCore core(Memory::external(), &io);
  bus.core = &core;
  core.set_registers(seed);
  constexpr std::array<Byte, 3> load{0xb6, 0xab, 0xcd};
  require(core.run_instruction(load).fault == Fault::memory_read && bus.bytes == 1,
          "Fixture establishes a real destructive-read failure before CPU reset");
  accept(core, vector, seed, bus.target);
  require(bus.bytes == 1 && bus.words == 1 && bus.unchanged,
          "CPU reset clears its fault without rolling back or replaying prior bus effects");
}

void missing_word() {
  Bus bus;
  Io io{.owner = &bus, .read = Bus::byte};
  ObservedCore core(Memory::external(), &io);
  bus.core = &core;
  core.set_registers(seed);
  const auto result = core.reset(vector);
  require(!result.accepted && result.fault == Fault::memory_read && result.address == vector &&
              result.access_size == 2 && core.registers() == seed && bus.bytes == 0 &&
              bus.words == 0,
          "Reset vectors require a qualified whole-word owner; no invented byte-read fallback");
}

void partial_window() {
  constexpr Address wrapped_vector = 0xffff;
  constexpr Address target = 0xabcd;
  constexpr std::array<Address, 2> parts{wrapped_vector, 0};
  for (const auto first : parts) {
    Bus bus{.target = target, .vector_address = wrapped_vector, .allow = true};
    constexpr Byte high = 0xab;
    constexpr Byte low = 0xcd;
    std::array<Byte, 1> known{first == wrapped_vector ? high : low};
    Io io{.owner = &bus, .read_word = Bus::word};
    ObservedCore core(Memory::window({.first = first, .bytes = known}), &io);
    bus.core = &core;
    core.set_registers(seed);
    accept(core, wrapped_vector, seed, target);
    require(bus.words == 1 && bus.observed == wrapped_vector && bus.unchanged,
            "Partial/wrapped vectors delegate the complete logical word to the bus owner");
  }
}
}

void bus_cases() {
  failure_and_recovery();
  prior_data_failure();
  missing_word();
  partial_window();
}
}
