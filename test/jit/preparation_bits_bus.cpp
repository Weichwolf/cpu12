// SPDX-License-Identifier: GPL-3.0-only
#include "bits_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "preparation_bits.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace cpu12::test::prepared_bits {
namespace {
enum class Phase : std::uint8_t { read, selected, write };

struct Event {
  Phase phase;
  Address address;
  Byte value, mask;
  bool operator==(const Event &) const = default;
};

struct Point {
  Byte data, ccr;
  unsigned refuse, yield;
};

struct Port {
  Core *core = nullptr;
  Registers before;
  Progress progress;
  std::array<Event, 2> events{};
  unsigned used = 0, refuse = 0, yield = 0;
  Byte data = 0;
  bool original = true;

  bool record(const Event event) noexcept {
    original = original && used < events.size() && core->registers() == before &&
               core->progress() == progress;
    if (used < events.size()) { events[used] = event; }
    ++used;
    const unsigned bit = event.phase == Phase::write ? 2 : 1;
    if (yield == bit) { core->request_yield(); }
    return (refuse & bit) == 0;
  }

  static bool read(void *owner, const Address at, Byte &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    value = port.data;
    return port.record({.phase = Phase::read, .address = at, .value = value, .mask = byte_mask});
  }

  static bool selected(void *owner, const MaskedByteRead access, Byte &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    value = port.data;
    return port.record(
        {.phase = Phase::selected, .address = access.address, .value = value, .mask = access.mask});
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.data = access.value;
    return port.record(
        {.phase = Phase::write, .address = access.address, .value = access.value, .mask = 0});
  }
};

RunResult failure(const Site &site, const Point point, const Byte value) {
  const bool read = (point.refuse & 1) != 0;
  if (!read && (site.operation.branch || (point.refuse & 2) == 0)) { return {}; }
  const auto mask = site.operation.branch ? site.mask : Byte{byte_mask};
  return {.fault = read ? Fault::memory_read : Fault::memory_write,
          .pc = site.pc,
          .opcode = site.code.bytes[0],
          .address = site.address(),
          .value = read ? point.data : value,
          .access_size = 1,
          .read_mask = read ? mask : Byte{0}};
}

void one(Core &core, Port &port, const Site &site, const Point point, const bool fallback) {
  const auto model = site.point({.data = point.data, .ccr = point.ccr});
  core.set_registers(model.before);
  port = {.core = &core,
          .before = model.before,
          .progress = core.progress(),
          .refuse = point.refuse,
          .yield = point.yield,
          .data = point.data};
  const auto compiled = core.statistics().compiled_entries;
  constexpr unsigned following = 6;
  const auto execution = core.run_until(port.progress.estimated_cycles + site.cycles() + following);
  const bool branch = site.operation.branch;
  const bool write = !branch && (point.refuse & 1) == 0;
  const auto value = static_cast<Byte>(
      write ? bits::modified(site.operation, {.data = point.data, .mask = site.mask}) : point.data);
  const bool refused = (point.refuse & 1) != 0 || (write && (point.refuse & 2) != 0);
  const bool yielded = point.yield == 1 || (write && point.yield == 2);
  State wanted{.registers = model.before, .failure = {}};
  unsigned retired = 0;
  unsigned elapsed = 0;
  Address last = site.pc;
  auto opcode = site.code.bytes[0];
  wanted.failure = failure(site, point, value);
  if (!refused) {
    wanted.registers = bits::expected(model, site.code.length);
    retired = yielded ? 1 : 3;
    elapsed = site.cycles() + (yielded ? 0 : following);
    if (!yielded) {
      last = wanted.registers.pc;
      opcode = idle[0];
    }
  }
  const bool selected = branch && !fallback;
  const std::array events{
      Event{.phase = selected ? Phase::selected : Phase::read,
            .address = site.address(),
            .value = point.data,
            .mask = selected ? site.mask : Byte{byte_mask}},
      write ? Event{.phase = Phase::write, .address = site.address(), .value = value, .mask = 0}
            : Event{}};
  require(execution.retired == retired && execution.fault == wanted.failure.fault &&
              execution.pc == last && execution.opcode == opcode && core.state() == wanted &&
              core.progress().estimated_cycles == port.progress.estimated_cycles + elapsed &&
              port.original && port.used == (write ? 2U : 1U) && port.events == events &&
              port.data == value && core.statistics().compiled_entries == compiled,
          "Prepared bits preserve actual selected/full read contracts, ordered side effects, "
          "staged flags/index and exact fault/yield prefixes");
}

std::uint64_t
mode(const Source &source, const NativeImage &image, const bool installed, const bool fallback) {
  auto code = source.bytes;
  Port port{.before = {}, .progress = {}};
  Io io{.owner = &port,
        .read = Port::read,
        .write = Port::write,
        .read_masked = fallback ? nullptr : Port::selected};
  Core core(Memory::window({.first = first, .bytes = code, .writable = false}), &io);
  if (installed) {
    core.install(image, variant);
  } else {
    const auto prepared = core.prepare(source.entries);
    require(prepared.failure.fault == Fault::none && prepared.unresolved_entries == 0 &&
                prepared.native_bytes != 0,
            "Graph preparation compiles bit operations without ISA or I/O execution");
  }
  require(port.used == 0, "Bit image compilation/installation/preparation performs no bus calls");
  std::uint64_t count = 0;
  constexpr std::array<Byte, 4> flags{0, 1, 0xa5, 0xff};
  for (const auto &site : source.sites) {
    for (const auto data : edges) {
      for (const auto ccr : flags) {
        for (unsigned refuse = 0; refuse < 4; ++refuse) {
          for (unsigned yield = 0; yield < 3; ++yield) {
            one(core,
                port,
                site,
                {.data = data, .ccr = ccr, .refuse = refuse, .yield = yield},
                fallback);
            ++count;
          }
        }
      }
    }
  }
  require(code == source.bytes, "Actual bit-bus effects retain the complete code image");
  return count;
}
}

void bus_cases() {
  const Source source(false);
  const auto image = compile(source, true);
  std::uint64_t count = 0;
  for (const bool installed : {false, true}) {
    for (const bool fallback : {false, true}) { count += mode(source, image, installed, fallback); }
  }
  std::cout << count << " independent bit-bus/refusal/yield cases passed.\n";
}
}
