// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_frames.hpp"
#include "interrupt_model.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace cpu12::test::image_frames {
namespace {
namespace frames = cpu12::test::interrupts;
constexpr Address saved_d = 0x3412;

struct Access {
  Address address = 0, value = 0;
  unsigned width = 0;
  bool write = false;
  bool operator==(const Access &) const = default;
};

struct Bus {
  Fixture &fixture;
  Core *core = nullptr;
  Registers before;
  Progress progress;
  unsigned calls = 0, failure = 16, yielding = 16, fetches = 0;
  bool unchanged = true;
  std::array<Access, 16> trace{};

  bool observe(const Access access) noexcept {
    unchanged &= core->registers() == before && core->progress() == progress &&
                 core->state().execution == ExecutionState::running &&
                 core->step().fault == Fault::reentrant && !core->resume_from_stop().accepted;
    if (calls < trace.size()) { trace[calls] = access; }
    if (calls == yielding) { core->request_yield(); }
    return calls++ != failure;
  }

  static bool byte_read(void *owner, const Address at, Byte &value) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    value = self.fixture.bytes[at];
    return self.observe({.address = at, .value = value, .width = 1});
  }

  static bool word_read(void *owner, const Address at, Address &value) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    value = static_cast<Address>((unsigned{self.fixture.bytes[at]} << byte_bits) |
                                 self.fixture.bytes[static_cast<Address>(at + 1U)]);
    return self.observe({.address = at, .value = value, .width = 2});
  }

  static bool byte_write(void *owner, const ByteWrite access) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    self.fixture.bytes[access.address] = access.value;
    return self.observe(
        {.address = access.address, .value = access.value, .width = 1, .write = true});
  }

  static bool word_write(void *owner, const WordWrite access) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    self.fixture.bytes[access.address] = static_cast<Byte>(access.value >> byte_bits);
    self.fixture.bytes[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value);
    return self.observe(
        {.address = access.address, .value = access.value, .width = 2, .write = true});
  }

  static bool fetch(void *owner, Address /*at*/, Byte & /*value*/) noexcept {
    ++static_cast<Bus *>(owner)->fetches;
    return false;
  }

  [[nodiscard]] Io io() {
    return {.owner = this,
            .read = byte_read,
            .write = byte_write,
            .read_word = word_read,
            .write_word = word_write,
            .fetch = fetch};
  }
};

std::array<Access, 5> sequence(const Registers before) {
  const auto at = [&](const int delta) { return static_cast<Address>(int{before.sp} + delta); };
  if (before.pc == rti) {
    return {{{.address = at(0), .value = frames::restored.ccr, .width = 1},
             {.address = at(1), .value = saved_d, .width = 2},
             {.address = at(3), .value = frames::restored.x, .width = 2},
             {.address = at(7), .value = frames::restored.pc, .width = 2},
             {.address = at(5), .value = frames::restored.y, .width = 2}}};
  }
  return {{{.address = at(-2), .value = sleeping(before).pc, .width = 2, .write = true},
           {.address = at(-4), .value = before.y, .width = 2, .write = true},
           {.address = at(-6), .value = before.x, .width = 2, .write = true},
           {.address = at(-8), .value = saved_d, .width = 2, .write = true},
           {.address = at(-9), .value = before.ccr, .width = 1, .write = true}}};
}

void apply(std::array<Byte, address_bytes> &memory, const Access access) {
  if (!access.write) { return; }
  memory[access.address] =
      static_cast<Byte>(access.width == 1 ? access.value : access.value >> byte_bits);
  if (access.width == 2) {
    memory[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value);
  }
}

void external_case(const NativeImage &image,
                   const Form form,
                   const unsigned phase,
                   const bool yielding) {
  Fixture fixture(external);
  const auto pc = form.pc;
  auto before = seed;
  before.pc = pc;
  if (pc == rti) {
    before.sp = frames::seed.sp;
    frames::put(fixture.bytes, before.sp, frames::literal);
  }
  Bus bus{.fixture = fixture,
          .before = before,
          .progress = {},
          .failure = yielding ? 16U : phase,
          .yielding = yielding ? phase : 16U};
  auto io = bus.io();
  Core core(fixture.memory(), &io);
  bus.core = &core;
  core.install(image, variant_id);
  core.set_registers(before);
  const auto compiled = core.statistics().compiled_entries;
  const auto accesses = sequence(before);
  auto memory = fixture.bytes;
  const bool accepted = yielding || phase == accesses.size();
  const auto calls = accepted ? accesses.size() : phase + 1;
  for (const auto access : std::span(accesses).first(calls)) { apply(memory, access); }
  const auto result = core.run(1);
  require(bus.unchanged && bus.calls == calls && bus.fetches == 0 && fixture.bytes == memory &&
              core.statistics().compiled_entries == compiled,
          "Prepared frame callbacks see original CPU/time, retain real effects and never fetch or "
          "compile");
  for (std::size_t index = 0; index < calls; ++index) {
    require(bus.trace[index] == accesses[index],
            "Prepared frames preserve all five actual logical accesses in their required order");
  }
  if (accepted) {
    const auto wanted = pc == rti ? frames::expected(before, frames::restored) : sleeping(before);
    require(result.retired == 1 && result.fault == Fault::none &&
                core.state() == State{.registers = wanted,
                                      .failure = {},
                                      .execution = form.execution,
                                      .returned_frame = pc == rti} &&
                core.progress().estimated_cycles == form.cycles,
            "A yield during a frame completes the accepted instruction before publishing its new "
            "state");
  } else {
    const auto fault = pc == rti ? Fault::memory_read : Fault::memory_write;
    const auto access = accesses[phase];
    require(result.retired == 0 && result.fault == fault && result.pc == pc &&
                result.opcode == form.opcode && result.address == access.address &&
                result.value == access.value && result.access_size == access.width &&
                core.registers() == before && core.state().execution == ExecutionState::running &&
                !core.state().returned_frame && core.progress() == Progress{},
            "Every refused prepared frame field preserves registers/time and reports exact "
            "operation diagnostics");
    require(core.step().fault == fault && core.run(16).fault == fault && bus.calls == calls &&
                fixture.bytes == memory,
            "A latched frame fault cannot replay earlier external transactions");
    bus.failure = 16;
    core.set_registers(before);
    require(core.step().retired == 1 && bus.calls == calls + accesses.size() && bus.unchanged,
            "An explicit new architectural boundary recovers a failed prepared frame without stale "
            "staging");
  }
}

void partial_case(const NativeImage &image, const Address pc) {
  Fixture fixture(partial);
  auto before = seed;
  before.pc = pc;
  if (pc == rti) {
    before.sp = frames::seed.sp;
    frames::put(fixture.bytes, before.sp, frames::literal);
    fixture.known.fill(1);
  }
  Bus bus{.fixture = fixture, .before = before, .progress = {}};
  auto io = bus.io();
  Core core(fixture.memory(), &io);
  bus.core = &core;
  core.install(image, variant_id);
  core.set_registers(before);
  auto memory = fixture.bytes;
  const auto accesses = sequence(before);
  for (const auto access : accesses) { apply(memory, access); }
  require(core.step().retired == 1 && bus.calls == 3 && bus.unchanged && fixture.bytes == memory,
          "Mixed tracked RAM and actual I/O stage each complete frame before committing CPU state");
  const auto start = pc == rti ? 0U : 2U;
  for (unsigned phase = 0; phase < bus.calls; ++phase) {
    require(bus.trace[phase] == accesses[start + phase],
            "An RTI word crossing ffff remains one external logical word even when its low byte is "
            "RAM");
  }
  if (pc != rti) {
    for (unsigned offset = 0; offset < 4; ++offset) {
      require(fixture.known[offset] == 1,
              "Prepared frame writes initialize actual tracked RAM bytes");
    }
  }
}
}

void bus_cases() {
  const auto image = compile(external);
  for (const auto form : forms) {
    for (unsigned phase = 0; phase <= 5; ++phase) { external_case(image, form, phase, false); }
    for (unsigned phase = 0; phase < 5; ++phase) { external_case(image, form, phase, true); }
  }
  const auto mixed = compile(partial);
  for (const auto pc : {wai, stop, rti}) { partial_case(mixed, pc); }
}
}
