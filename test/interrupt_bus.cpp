// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "interrupt_model.hpp"

#include <array>
#include <span>

namespace cpu12::test::interrupts {
namespace {
struct Read {
  Address address = 0;
  unsigned width = 0;
  bool operator==(const Read &) const = default;
};

struct Bus {
  Image image{};
  Core *core = nullptr;
  unsigned calls = 0, failure = 5, writes = 0;
  bool unchanged = true;
  std::array<Read, 16> trace{};

  bool observed(const Read access) noexcept {
    unchanged &= core->registers() == seed;
    if (calls < trace.size()) { trace[calls] = access; }
    return calls++ != failure;
  }

  static bool byte(void *owner, const Address address, Byte &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    value = bus.image[address];
    return bus.observed({.address = address, .width = 1});
  }

  static bool word(void *owner, const Address address, Address &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    constexpr unsigned byte_bits = 8;
    value = static_cast<Address>((static_cast<unsigned>(bus.image[address]) << byte_bits) |
                                 bus.image[static_cast<Address>(address + 1U)]);
    return bus.observed({.address = address, .width = 2});
  }

  static bool write(void *owner, ByteWrite /*access*/) noexcept {
    ++static_cast<Bus *>(owner)->writes;
    return false;
  }
};

constexpr std::array<Read, 5> sequence{
    {{.address = seed.sp, .width = 1},
     {.address = static_cast<Address>(seed.sp + 1U), .width = 2},
     {.address = static_cast<Address>(seed.sp + 3U), .width = 2},
     {.address = static_cast<Address>(seed.sp + 7U), .width = 2},
     {.address = static_cast<Address>(seed.sp + 5U), .width = 2}}};

constexpr std::array<Address, 5> diagnostic_values{0xd7, 0x3412, 0x5678, 0xdef0, 0x9abc};

void all_external() {
  for (unsigned phase = 0; phase <= sequence.size(); ++phase) {
    Bus bus{.failure = phase};
    put(bus.image, seed.sp, literal);
    Io io{.owner = &bus, .read = Bus::byte, .write = Bus::write, .read_word = Bus::word};
    Core core(Memory::external(), &io);
    bus.core = &core;
    core.set_registers(seed);
    if (phase == sequence.size()) {
      accept(core, seed, expected(seed, restored));
    } else {
      const auto result = core.run_instruction(rti);
      require(result.retired == 0 && result.fault == Fault::memory_read && result.pc == seed.pc &&
                  result.opcode == rti[0] && result.address == sequence[phase].address &&
                  result.value == diagnostic_values[phase] &&
                  result.access_size == sequence[phase].width && core.registers() == seed,
              "A refused RTI phase reports the actual access and preserves every CPU register");
      core.clear_code_cache();
      require(core.run_instruction(rti).fault == Fault::memory_read && bus.calls == phase + 1,
              "Failed RTI does not replay earlier external reads across cache clearing");
    }
    const auto calls = phase < sequence.size() ? phase + 1 : sequence.size();
    require(bus.calls == calls && bus.writes == 0 && bus.unchanged &&
                core.statistics().read_callbacks == calls,
            "External observers see the original state until every RTI read has succeeded");
    for (unsigned index = 0; index < calls; ++index) {
      require(bus.trace[index] == sequence[index],
              "RTI reads CCR,B:A,X,PC,Y in documented logical access order and width");
    }
    if (phase < sequence.size()) {
      bus.failure = 16;
      core.set_registers(seed);
      accept(core, seed, expected(seed, restored));
      require(bus.calls == calls + sequence.size() && bus.unchanged && bus.writes == 0,
              "An explicit fresh boundary recovers native RTI without host-frame leakage");
    }
  }
}

void missing_word() {
  Bus bus;
  put(bus.image, seed.sp, literal);
  Io io{.owner = &bus, .read = Bus::byte};
  Core core(Memory::external(), &io);
  bus.core = &core;
  core.set_registers(seed);
  const auto result = core.run_instruction(rti);
  require(result.fault == Fault::memory_read && result.retired == 0 &&
              result.address == sequence[1].address && result.access_size == 2 &&
              core.registers() == seed && bus.calls == 1 && core.statistics().read_callbacks == 1,
          "Missing wide bus refuses instead of inventing two accepted byte reads");
}

void partial_window() {
  Bus bus;
  put(bus.image, seed.sp, literal);
  Io io{.owner = &bus, .read = Bus::byte, .read_word = Bus::word};
  Core core(Memory::unified(std::span(bus.image).first(8)), &io);
  bus.core = &core;
  core.set_registers(seed);
  accept(core, seed, expected(seed, restored));
  require(bus.calls == 3 && bus.unchanged && core.statistics().read_callbacks == 3,
          "Partial RAM stages the complete RTI before CPU publication");
  for (unsigned phase = 0; phase < 3; ++phase) {
    require(bus.trace[phase] == sequence[phase],
            "A word crossing ffff into known RAM remains one complete external wide access");
  }
}
}

void bus_cases() {
  all_external();
  missing_word();
  partial_window();
}
}
