// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_support.hpp"
#include "isa.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace cpu12::test::image {
namespace {
template <class Action> bool refused(Action action) noexcept {
  try {
    action();
  } catch (const std::logic_error &) { return true; } catch (...) {
    return false;
  }
  return false;
}

struct FrameBus {
  Core *core = nullptr;
  const NativeImage *image = nullptr;
  Fixture *fixture = nullptr;
  WordWrite transaction{};
  unsigned writes = 0;
  bool install_refused = false, remap_refused = false, prepare_refused = false, run_refused = false;

  static bool write(void *owner, const WordWrite value) noexcept {
    auto &self = *static_cast<FrameBus *>(owner);
    self.transaction = value;
    ++self.writes;
    self.install_refused = refused([&] { self.core->install(*self.image, first_id); });
    self.remap_refused = refused([&] { self.core->set_memory(self.fixture->memory()); });
    const std::array entries{entry};
    self.prepare_refused = self.core->prepare(entries).failure.fault == Fault::reentrant;
    self.run_refused = self.core->run(1).fault == Fault::reentrant;
    return false;
  }
};

void frame_boundary(const NativeImage &image) {
  Fixture fixture{Source(0x80)};
  FrameBus bus{.image = &image, .fixture = &fixture};
  Io io{.owner = &bus, .write_word = FrameBus::write};
  Core core(fixture.memory(), &io);
  bus.core = &core;
  constexpr Registers before{.pc = entry, .sp = entry + 9};
  core.set_registers(before);
  core.install(image, first_id);
  const auto compiled = core.statistics().compiled_entries;
  const auto progress = core.progress();
  const auto memory = fixture.bytes;
  const auto result = core.interrupt(interrupts[0]);
  require(!result.accepted && result.fault == Fault::memory_write && result.access_size == 2 &&
              result.address == before.sp - 2 && result.value == before.pc && bus.writes == 1 &&
              bus.transaction.address == result.address && bus.transaction.value == result.value &&
              bus.install_refused && bus.remap_refused && bus.prepare_refused && bus.run_refused &&
              core.registers() == before && core.progress() == progress &&
              fixture.bytes == memory && core.statistics().compiled_entries == compiled,
          "A prepared real IRQ write must preserve the exact fault boundary and reject "
          "reentrant installation/remapping/preparation/execution");
}

struct Port {
  static constexpr Address address = 0x1234;
  static constexpr Byte value = 0x5a;
  Core *core = nullptr;
  unsigned reads = 0, fetches = 0;
  Address fetched_address = 0;

  static bool read(void *owner, const Address at, Byte &out) noexcept {
    auto &self = *static_cast<Port *>(owner);
    ++self.reads;
    if (at != address) { return false; }
    out = value;
    self.core->request_yield();
    return true;
  }

  static bool fetch(void *owner, const Address at, Byte &out) noexcept {
    auto &self = *static_cast<Port *>(owner);
    ++self.fetches;
    self.fetched_address = at;
    out = 0;
    return false;
  }
};

void io_yield() {
  auto code = isa::program(isa::opcode<isa::Opcode::ldaa_extended>(),
                           std::array<Byte, 2>{static_cast<Byte>(Port::address >> byte_bits),
                                               static_cast<Byte>(Port::address)},
                           std::array<Byte, 2>{bra, backedge});
  const std::array spans{CodeSpan{.address = entry, .words = code}};
  constexpr std::array<Address, 1> entries{entry};
  const std::array variants{CodeVariant{.id = first_id, .code = spans, .entries = entries}};
  const std::array regions{RegionLayout{.first = entry, .size = code.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = first_id, .memory = regions}};
  const auto image = Core::compile(
      {.variants = variants},
      {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
  const std::array grants{Ram{.first = entry, .bytes = code, .writable = false}};
  Port port;
  Io io{.owner = &port, .read = Port::read, .fetch = Port::fetch};
  Core core(Memory::mapped(grants), &io);
  port.core = &core;
  core.install(image, first_id);
  constexpr Registers before{.pc = entry, .d = 0x1234, .ccr = 0x20};
  constexpr Registers wanted{.pc = entry + 3, .d = 0x5a34, .ccr = 0x20};
  core.set_registers(before);
  const auto compiled = core.statistics().compiled_entries;
  constexpr unsigned budget = 64;
  require(core.run(budget).retired == 1 &&
              core.state() == State{.registers = wanted, .failure = {}} &&
              core.progress() == Progress{.estimated_cycles = 3} && port.reads == 1 &&
              port.fetches == 0,
          "Prepared real I/O must return after the accepted yielding instruction with exact state");
  require(core.run(budget).retired == budget && core.registers() == wanted && port.reads == 1 &&
              port.fetches == 0 &&
              core.progress().estimated_cycles == 3 + (std::uint64_t{budget} * 3) &&
              core.statistics().compiled_entries == compiled,
          "The next owner grant must resume original prepared code without replaying I/O or "
          "translation");
  core.set_registers({.pc = Port::address});
  require(core.step().fault == Fault::unprepared_execution && port.reads == 1 &&
              port.fetches == 0 && core.statistics().compiled_entries == compiled,
          "Strict unknown entries cannot invoke fetch/I/O callbacks to discover new code");
}
}

void image_io() {
  const auto image = compile();
  frame_boundary(image);
  io_yield();
}
}
