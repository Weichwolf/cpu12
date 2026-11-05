// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "irq_model.hpp"

#include <array>
#include <span>

namespace cpu12::test::irqs {
namespace {
struct Transfer {
  bool write = false;
  Address address = 0, value = 0;
  unsigned width = 2;
  bool operator==(const Transfer &) const = default;
};

constexpr std::array<Transfer, 6> sequence{
    {{.address = vector, .value = handler},
     {.write = true, .address = static_cast<Address>(seed.sp - 2), .value = seed.pc},
     {.write = true, .address = static_cast<Address>(seed.sp - 4), .value = seed.y},
     {.write = true, .address = static_cast<Address>(seed.sp - 6), .value = seed.x},
     {.write = true, .address = static_cast<Address>(seed.sp - 8), .value = 0x3412},
     {.write = true, .address = static_cast<Address>(seed.sp - 9), .value = seed.ccr, .width = 1}}};

struct Bus {
  Image image{};
  Core *core = nullptr;
  unsigned calls = 0, failure = 6;
  bool intact = true;
  std::array<Transfer, 16> trace{};

  bool observe(const Transfer transfer) noexcept {
    intact &= core->registers() == seed;
    const auto nested = core->interrupt({.vector = vector});
    intact &= !nested.accepted && nested.fault == Fault::reentrant;
    if (calls < trace.size()) { trace[calls] = transfer; }
    return calls++ != failure;
  }

  static bool read(void *owner, const Address address, Address &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    constexpr unsigned bits = 8;
    value = static_cast<Address>((static_cast<unsigned>(bus.image[address]) << bits) |
                                 bus.image[static_cast<Address>(address + 1U)]);
    return bus.observe({.address = address, .value = value});
  }

  static bool word(void *owner, const WordWrite transfer) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    constexpr unsigned bits = 8;
    constexpr unsigned mask = 0xff;
    bus.image[transfer.address] = static_cast<Byte>(transfer.value >> bits);
    bus.image[static_cast<Address>(transfer.address + 1U)] =
        static_cast<Byte>(transfer.value & mask);
    return bus.observe({.write = true, .address = transfer.address, .value = transfer.value});
  }

  static bool byte(void *owner, const ByteWrite transfer) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    bus.image[transfer.address] = transfer.value;
    return bus.observe(
        {.write = true, .address = transfer.address, .value = transfer.value, .width = 1});
  }
};

void failures() {
  for (unsigned phase = 0; phase <= sequence.size(); ++phase) {
    Bus bus{.failure = phase};
    prepare_vector(bus.image, vector);
    Io io{.owner = &bus, .write = Bus::byte, .read_word = Bus::read, .write_word = Bus::word};
    Core core(Memory::external(), &io);
    bus.core = &core;
    core.set_registers(seed);
    if (phase == sequence.size()) {
      accept(core, seed, {.vector = vector}, bus.image);
    } else {
      const auto result = core.interrupt({.vector = vector});
      const auto cause = phase == 0 ? Fault::memory_read : Fault::memory_write;
      require(
          !result.accepted && result.fault == cause && result.pc == seed.pc &&
              result.address == sequence[phase].address && result.value == sequence[phase].value &&
              result.access_size == sequence[phase].width && core.registers() == seed,
          "Rejected IRQ vector/frame phase reports actual access without partial CPU publication");
      require(
          core.interrupt({.vector = vector}).fault == cause && bus.calls == phase + 1 &&
              core.step().fault == cause,
          "Interrupt failure latches and replays neither vector reads nor partial stack writes");
    }
    const auto calls = phase < sequence.size() ? phase + 1 : sequence.size();
    require(bus.calls == calls && bus.intact,
            "Every IRQ callback sees original registers and rejects recursive interrupt entry");
    for (unsigned index = 0; index < calls; ++index) {
      require(bus.trace[index] == sequence[index],
              "IRQ performs vector read then PC,Y,X,B:A,CCR writes in independent expected order");
    }
    if (phase < sequence.size()) {
      for (unsigned index = 1; index <= phase; ++index) {
        const auto transfer = sequence[index];
        constexpr unsigned bits = 8;
        if (transfer.width == 2) {
          require(bus.image[transfer.address] == transfer.value >> bits,
                  "Partial external IRQ writes remain actual effects after failure");
        } else {
          require(bus.image[transfer.address] == transfer.value,
                  "Even the rejecting CCR write's actual external effect is retained");
        }
      }
      bus.failure = 16;
      core.set_registers(seed);
      accept(core, seed, {.vector = vector}, bus.image);
    }
  }
}

void partial_ram() {
  Bus bus;
  prepare_vector(bus.image, vector);
  constexpr Address first = seed.sp - 9;
  Io io{.owner = &bus, .write = Bus::byte, .read_word = Bus::read, .write_word = Bus::word};
  Core core(Memory::window({.first = first, .bytes = std::span(bus.image).subspan(first, 9)}), &io);
  bus.core = &core;
  core.set_registers(seed);
  accept(core, seed, {.vector = vector}, bus.image);
  require(bus.calls == 1 && bus.trace[0] == sequence[0] && bus.intact &&
              core.statistics().read_callbacks == 1 && core.statistics().write_callbacks == 0,
          "An external vector with known-RAM frame still stages and commits a real target");
}

void missing_vector() {
  Core core;
  core.set_registers(seed);
  const auto result = core.interrupt({.vector = vector});
  require(!result.accepted && result.fault == Fault::memory_read && result.address == vector &&
              result.access_size == 2 && core.registers() == seed &&
              !core.interrupt_ready(InterruptKind::maskable) &&
              core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "An eligible IRQ without a vector provider fails visibly before any stack write");
}
}

void bus_cases() {
  failures();
  partial_ram();
  missing_vector();
}
}
