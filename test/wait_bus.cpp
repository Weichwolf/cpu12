// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "wait_model.hpp"

#include <array>
#include <span>
#include <stdexcept>

namespace cpu12::test::waits {
namespace {
struct Transfer {
  Address address, value;
  bool word;
  bool operator==(const Transfer &) const = default;
};

struct Bus {
  Image image{};
  Core *core = nullptr;
  Form form = forms[0];
  unsigned writes = 0, reads = 0, fetches = 0, failure = 5;
  bool intact = true, vector_allowed = true;
  std::array<Transfer, 5> trace{};

  bool observe(const Transfer transfer) noexcept {
    intact &= core->registers() == seed && core->state() == ExecutionState::running;
    intact &= core->step().fault == Fault::reentrant &&
              core->resume_from_stop().fault == Fault::reentrant;
    bool snapshot_refused = false;
    try {
      static_cast<void>(core->checkpoint());
    } catch (const std::logic_error &) { snapshot_refused = true; }
    bool restore_refused = false;
    try {
      core->restore({.registers = seed, .failure = {}});
    } catch (const std::logic_error &) { restore_refused = true; }
    intact &= snapshot_refused && restore_refused;
    if (writes < trace.size()) { trace[writes] = transfer; }
    return writes++ != failure;
  }

  static bool word(void *owner, const WordWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    constexpr unsigned bits = 8;
    constexpr unsigned mask = 0xff;
    bus.image[access.address] = static_cast<Byte>(access.value >> bits);
    bus.image[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value & mask);
    return bus.observe({.address = access.address, .value = access.value, .word = true});
  }

  static bool byte(void *owner, const ByteWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    bus.image[access.address] = access.value;
    return bus.observe({.address = access.address, .value = access.value, .word = false});
  }

  static bool read(void *owner, const Address address, Address &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.reads;
    value = handler;
    bus.intact &= core_idle(bus) && bus.core->registers() == sleeping(seed, bus.form);
    return bus.vector_allowed && address == vector;
  }

  static bool fetch(void *owner, Address /*address*/, Byte &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.fetches;
    value = 0;
    return false;
  }

  static bool core_idle(const Bus &bus) noexcept { return bus.core->state() == bus.form.state; }
};

void write_phases() {
  for (const auto form : forms) {
    const std::array<Transfer, 5> expected{
        {{.address = 2, .value = static_cast<Address>(seed.pc + form.length), .word = true},
         {.address = 0, .value = seed.y, .word = true},
         {.address = 0xfffe, .value = seed.x, .word = true},
         {.address = 0xfffc, .value = 0x3412, .word = true},
         {.address = 0xfffb, .value = seed.ccr, .word = false}}};
    for (unsigned phase = 0; phase < expected.size(); ++phase) {
      Bus bus{.form = form, .failure = phase};
      Io io{.owner = &bus, .write = Bus::byte, .write_word = Bus::word};
      Core core(Memory::external(), &io);
      bus.core = &core;
      core.set_registers(seed);
      const auto result = core.run_instruction(form.bytes());
      require(result.fault == Fault::memory_write && result.retired == 0 &&
                  result.state == ExecutionState::running && result.pc == seed.pc &&
                  result.address == expected[phase].address &&
                  result.value == expected[phase].value &&
                  result.access_size == (expected[phase].word ? 2 : 1) &&
                  core.registers() == seed && core.state() == ExecutionState::running,
              "Refused wait write reports the actual phase and leaves all CPU/wait state intact");
      require(
          bus.intact && bus.writes == phase + 1,
          "Real callbacks observe original CPU state and refuse reentrant execution/checkpoints");
      for (unsigned index = 0; index <= phase; ++index) {
        require(bus.trace[index] == expected[index],
                "Wait writes nextPC,Y,X,B:A,CCR in independently specified logical order");
        const auto low =
            static_cast<Address>(expected[index].address + (expected[index].word ? 1U : 0U));
        constexpr unsigned mask = 0xff;
        require(bus.image[low] == (expected[index].value & mask),
                "Earlier and rejecting write effects remain in actual external storage");
      }
      const auto failed = core.checkpoint();
      core.clear_code_cache();
      require(core.run(idle_budget).fault == Fault::memory_write && bus.writes == phase + 1,
              "A failed wait cannot replay its actual partial writes");
      core.set_registers(seed);
      core.restore(failed);
      require(core.step().fault == Fault::memory_write && bus.writes == phase + 1,
              "Restoring the fault boundary preserves exact no-replay behavior");
      core.set_registers(seed);
      bus.writes = 0;
      bus.failure = 5;
      enter(core, bus.image, seed, form);
    }
  }
}

void wake_phases() {
  for (const auto form : forms) {
    Bus bus{.form = form};
    Io io{.owner = &bus,
          .write = Bus::byte,
          .read_word = Bus::read,
          .write_word = Bus::word,
          .fetch = Bus::fetch};
    Core core(Memory::external(), &io);
    bus.core = &core;
    core.set_registers(seed);
    enter(core, bus.image, seed, form);
    const auto saved = core.checkpoint();
    core.clear_code_cache();
    require(core.step().state == form.state && core.run(idle_budget).state == form.state &&
                core.run_instruction(nop).state == form.state && bus.fetches == 0 &&
                bus.reads == 0 && bus.writes == 5,
            "Idle CPU accessors do not touch missing/destructive instruction or data providers");
    bus.vector_allowed = false;
    const auto failed = core.interrupt({.vector = vector});
    require(!failed.accepted && failed.fault == Fault::memory_read && failed.address == vector &&
                failed.value == handler && failed.access_size == 2 &&
                core.registers() == saved.registers && core.state() == form.state &&
                bus.writes == 5 && bus.reads == 1 && bus.intact,
            "Refused wake vector leaves the genuine sleep frame and CPU idle state unchanged");
    require(core.step().fault == Fault::memory_read &&
                core.interrupt({.vector = vector}).fault == Fault::memory_read && bus.reads == 1,
            "Wake vector failure latches without a second read or frame write");
    bus.vector_allowed = true;
    core.restore(saved);
    require(core.interrupt({.vector = vector}).accepted && bus.reads == 2 && bus.writes == 5 &&
                core.registers().sp == saved.registers.sp && core.registers().pc == handler &&
                core.state() == ExecutionState::running && bus.intact,
            "Coordinated recovery resumes through one real vector and reuses the existing frame");
  }
}

void partial_window() {
  Bus bus;
  Io io{.owner = &bus, .write = Bus::byte, .write_word = Bus::word};
  Core core(Memory::unified(std::span(bus.image).first(3)), &io);
  bus.core = &core;
  core.set_registers(seed);
  enter(core, bus.image, seed, forms[0]);
  require(bus.writes == 4 && bus.trace[0].address == 2 && bus.trace[0].word && bus.intact,
          "A stack word crossing a known RAM seam delegates the whole logical write");
}
}

void bus_cases() {
  write_phases();
  wake_phases();
  partial_window();
}
}
