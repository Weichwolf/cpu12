// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "move_model.hpp"
#include "preparation_move.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace cpu12::test::prepared_move {
namespace {
struct Event {
  bool write = false;
  unsigned width = 0;
  Address address = 0, value = 0;
  bool operator==(const Event &) const = default;
};

struct Point {
  unsigned data;
  Byte ccr;
  unsigned refuse, yield;
};

struct Port {
  std::array<Byte, address_bytes> bytes{};
  Core *core = nullptr;
  Registers before;
  Progress progress;
  std::array<Event, 2> events{};
  unsigned used = 0, refuse = 0, yield = 0;
  bool original = true;

  bool record(const Event event) noexcept {
    original = original && used < events.size() && core->registers() == before &&
               core->progress() == progress;
    if (used < events.size()) { events[used] = event; }
    ++used;
    const unsigned phase = event.write ? 2 : 1;
    if (yield == phase) { core->request_yield(); }
    return (refuse & phase) == 0;
  }

  static bool read(void *owner, const Address at, Byte &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    value = port.bytes[at];
    return port.record({.width = 1, .address = at, .value = value});
  }

  static bool read_word(void *owner, const Address at, Address &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    value = static_cast<Address>((unsigned{port.bytes[at]} << byte_bits) |
                                 port.bytes[static_cast<Address>(at + 1)]);
    return port.record({.width = 2, .address = at, .value = value});
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.bytes[access.address] = access.value;
    return port.record(
        {.write = true, .width = 1, .address = access.address, .value = access.value});
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.bytes[access.address] = static_cast<Byte>(access.value >> byte_bits);
    const bool accepted =
        port.record({.write = true, .width = 2, .address = access.address, .value = access.value});
    if (accepted) {
      port.bytes[static_cast<Address>(access.address + 1)] = static_cast<Byte>(access.value);
    }
    return accepted;
  }
};

struct Model {
  State state;
  std::array<Event, 2> events{};
  unsigned count = 0, retired = 0, elapsed = 0;
  Address last = 0;
  Byte opcode = 0, high = 0, low = 0;
};

RunResult failure(const Site &site, const Point point, const bool read) {
  return {.fault = read ? Fault::memory_read : Fault::memory_write,
          .pc = site.point.before.pc,
          .opcode = site.plan.code.bytes[0],
          .address = read ? site.plan.source : site.plan.destination,
          .value = static_cast<Address>(point.data),
          .access_size = static_cast<std::uint8_t>(site.point.word ? 2 : 1),
          .read_mask = static_cast<Byte>(read && !site.point.word ? byte_mask : 0)};
}

Model predict(const Site &site, const Point point, const std::array<Byte, 2> before) {
  auto registers = site.point.before;
  registers.ccr = point.ccr;
  Model model{.state = {.registers = registers, .failure = {}},
              .last = registers.pc,
              .opcode = site.plan.code.bytes[0],
              .high = before[0],
              .low = before[1]};
  const bool read = site.point.source.mode != move::Mode::immediate;
  const bool failed_read = read && (point.refuse & 1) != 0;
  const bool failed_write = !failed_read && (point.refuse & 2) != 0;
  const bool yielded = (read && point.yield == 1) || point.yield == 2;
  const unsigned width = site.point.word ? 2 : 1;
  if (read) {
    model.events[model.count++] = {
        .width = width, .address = site.plan.source, .value = static_cast<Address>(point.data)};
  }
  if (!failed_read) {
    model.events[model.count++] = {.write = true,
                                   .width = width,
                                   .address = site.plan.destination,
                                   .value = static_cast<Address>(point.data)};
    model.high = static_cast<Byte>(site.point.word ? point.data >> byte_bits : point.data);
    if (site.point.word && !failed_write) { model.low = static_cast<Byte>(point.data); }
  }
  if (failed_read || failed_write) {
    model.state.failure = failure(site, point, failed_read);
  } else {
    model.state.registers = site.plan.wanted;
    model.state.registers.ccr = point.ccr;
    model.retired = yielded ? 1 : 3;
    constexpr unsigned following = 6;
    model.elapsed = site.cycles() + (yielded ? 0 : following);
    if (!yielded) {
      model.last = site.plan.wanted.pc;
      model.opcode = idle[0];
    }
  }
  return model;
}

void one(Core &core, Port &port, const Site &site, Point point) {
  point.data &= site.point.word ? word_mask : byte_mask;
  if (site.point.source.mode == move::Mode::immediate) { point.data = site.point.data; }
  auto registers = site.point.before;
  registers.ccr = point.ccr;
  core.set_registers(registers);
  port.before = registers;
  port.progress = core.progress();
  port.events = {};
  port.used = 0;
  port.refuse = point.refuse;
  port.yield = point.yield;
  port.original = true;
  constexpr Byte high = 0x5a;
  constexpr Byte low = 0xa5;
  const auto destination = site.plan.destination;
  const auto next = static_cast<Address>(destination + 1);
  port.bytes[destination] = high;
  port.bytes[next] = low;
  if (site.point.source.mode != move::Mode::immediate) {
    port.bytes[site.plan.source] =
        static_cast<Byte>(site.point.word ? point.data >> byte_bits : point.data);
    if (site.point.word) {
      port.bytes[static_cast<Address>(site.plan.source + 1)] = static_cast<Byte>(point.data);
    }
  }
  const auto model = predict(site, point, {port.bytes[destination], port.bytes[next]});
  const auto compiled = core.statistics().compiled_entries;
  constexpr unsigned following = 6;
  const auto execution = core.run_until(port.progress.estimated_cycles + site.cycles() + following);
  require(execution.retired == model.retired && execution.fault == model.state.failure.fault &&
              execution.pc == model.last && execution.opcode == model.opcode &&
              core.state() == model.state &&
              core.progress().estimated_cycles == port.progress.estimated_cycles + model.elapsed &&
              port.original && port.used == model.count && port.events == model.events &&
              port.bytes[destination] == model.high && port.bytes[next] == model.low &&
              core.statistics().compiled_entries == compiled,
          "Prepared MOV preserves actual byte/word phase ordering, partial failed writes, staged "
          "indices and exact fault/yield prefixes");
  if (model.state.failure.fault != Fault::none) {
    require(core.run(1).fault == model.state.failure.fault && port.events == model.events &&
                port.used == model.count && core.state() == model.state &&
                core.statistics().compiled_entries == compiled,
            "A latched MOV fault cannot repeat actual source or destination effects");
  }
}

std::uint64_t mode(const Source &source, const NativeImage &image, const bool installed) {
  auto code = source.bytes;
  Port port{.before = {}, .progress = {}};
  Io io{.owner = &port,
        .read = Port::read,
        .write = Port::write,
        .read_word = Port::read_word,
        .write_word = Port::write_word};
  Core core(Memory::window({.first = first, .bytes = code, .writable = false}), &io);
  port.core = &core;
  if (installed) {
    core.install(image, variant);
  } else {
    const auto prepared = core.prepare(source.entries);
    require(prepared.failure.fault == Fault::none && prepared.unresolved_entries == 0 &&
                prepared.prepared_entries == source.sites.size() * 2 && prepared.native_bytes != 0,
            "External MOV graph preparation retains all entries without runtime translation");
  }
  require(port.used == 0, "MOV compilation/preparation/installation performs no real I/O");
  std::uint64_t count = 0;
  constexpr std::array<Byte, 4> flags{0, 1, 0xa5, 0xff};
  for (const auto &site : source.sites) {
    for (const auto data : edges) {
      for (const auto ccr : flags) {
        for (unsigned refuse = 0; refuse < 4; ++refuse) {
          for (unsigned yield = 0; yield < 3; ++yield) {
            one(core, port, site, {.data = data, .ccr = ccr, .refuse = refuse, .yield = yield});
            ++count;
          }
        }
      }
    }
  }
  require(code == source.bytes, "Actual MOV I/O effects retain declared executable bytes");
  return count;
}
}

void bus_cases() {
  const Source source(Group::external);
  const auto image = compile(source, true);
  const auto count = mode(source, image, true) + mode(source, image, false);
  std::cout << count << " independent prepared MOV I/O/fault/yield cases passed.\n";
}
}
