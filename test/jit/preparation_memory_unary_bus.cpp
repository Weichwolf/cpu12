// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "preparation_memory_unary.hpp"
#include "unary_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace cpu12::test::prepared_memory_unary {
namespace {
enum class Phase : std::uint8_t { pointer = 1, read = 2, write = 4 };

struct Event {
  Phase phase;
  Address address, value;
  bool operator==(const Event &) const = default;
};

struct Port {
  Core *core = nullptr;
  Registers before;
  Progress progress;
  std::array<Event, 3> events{};
  unsigned used = 0, refuse = 0, yield = 0;
  Byte data = 0;
  bool original = true;

  bool record(const Event event) noexcept {
    original = original && used < events.size() && core->registers() == before &&
               core->progress() == progress;
    if (used < events.size()) { events[used] = event; }
    ++used;
    const auto bit = static_cast<unsigned>(event.phase);
    if (yield == bit) { core->request_yield(); }
    return (refuse & bit) == 0;
  }

  static bool read_word(void *owner, const Address at, Address &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    value = operand;
    return port.record({.phase = Phase::pointer, .address = at, .value = value});
  }

  static bool read(void *owner, const Address at, Byte &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    value = port.data;
    return port.record({.phase = Phase::read, .address = at, .value = value});
  }

  static bool write(void *owner, const ByteWrite value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.data = value.value;
    return port.record({.phase = Phase::write, .address = value.address, .value = value.value});
  }
};

struct Model {
  std::array<Event, 3> events{};
  unsigned count = 0;
  RunResult failure;
  Byte data;
  bool yielded = false;
};

struct Point {
  Byte data, ccr;
  unsigned refuse, yield;
};

Model predict(const Site site, const Registers before, const Point point) {
  Model model{.failure = {}, .data = point.data};
  const auto accept = [&](const Event event) {
    model.events[model.count++] = event;
    model.yielded |= point.yield == static_cast<unsigned>(event.phase);
    if (event.phase == Phase::write) { model.data = static_cast<Byte>(event.value); }
    if ((point.refuse & static_cast<unsigned>(event.phase)) == 0) { return true; }
    const bool write = event.phase == Phase::write;
    const auto width = static_cast<std::uint8_t>(event.phase == Phase::pointer ? 2 : 1);
    model.failure = {.fault = write ? Fault::memory_write : Fault::memory_read,
                     .pc = site.pc,
                     .opcode = site.code[0],
                     .address = event.address,
                     .value = event.value,
                     .access_size = width,
                     .read_mask = static_cast<Byte>(!write && width == 1 ? byte_mask : 0)};
    return false;
  };
  if (site.form == Form::indirect &&
      !accept({.phase = Phase::pointer, .address = pointer, .value = operand})) {
    return model;
  }
  if (site.operation.read &&
      !accept({.phase = Phase::read, .address = operand, .value = point.data})) {
    return model;
  }
  if (site.operation.write) {
    const auto result = unary::expected({.function = site.operation.function,
                                         .data = point.data,
                                         .ccr = before.ccr,
                                         .word = false});
    static_cast<void>(accept(
        {.phase = Phase::write, .address = operand, .value = static_cast<Address>(result.data)}));
  }
  return model;
}

void one(Core &core, Port &port, const Site site, const Point point) {
  const auto before = seed(site, point.ccr);
  core.set_registers(before);
  port = {.core = &core,
          .before = before,
          .progress = core.progress(),
          .refuse = point.refuse,
          .yield = point.yield,
          .data = point.data};
  const auto model = predict(site, before, point);
  const auto compiled = core.statistics().compiled_entries;
  constexpr unsigned following_cycles = 6;
  const auto execution =
      core.run_until(port.progress.estimated_cycles + cycles(site) + following_cycles);
  State wanted{.registers = before, .failure = model.failure};
  unsigned retired = 0;
  unsigned elapsed = 0;
  Address last = site.pc;
  auto opcode = site.code[0];
  if (model.failure.fault == Fault::none) {
    wanted.registers = expected(before, site, point.data);
    retired = model.yielded ? 1 : 3;
    elapsed = cycles(site) + (model.yielded ? 0 : following_cycles);
    if (!model.yielded) {
      last = static_cast<Address>(site.pc + site.length);
      opcode = idle[0];
    }
  }
  require(execution.retired == retired && execution.fault == model.failure.fault &&
              execution.pc == last && execution.opcode == opcode && core.state() == wanted &&
              core.progress().estimated_cycles == port.progress.estimated_cycles + elapsed &&
              port.original && port.used == model.count && port.events == model.events &&
              port.data == model.data && core.statistics().compiled_entries == compiled,
          "Prepared memory unary retains each actual bus effect, ordered phase, staged state and "
          "exact fault/yield boundary");
}

std::uint64_t mode(const Source &source, const NativeImage &image, const bool installed) {
  auto code = source.bytes;
  Port port{.before = {}, .progress = {}};
  Io io{.owner = &port, .read = Port::read, .write = Port::write, .read_word = Port::read_word};
  Core core(Memory::window({.first = entry, .bytes = code, .writable = false}), &io);
  if (installed) {
    core.install(image, variant_id);
  } else {
    const auto report = core.prepare(source.entries);
    require(report.failure.fault == Fault::none &&
                report.prepared_entries == source.sites.size() * 2 &&
                report.unresolved_entries == 0 && report.native_bytes != 0,
            "External memory unary preparation cannot defer ISA compilation into execution");
  }
  require(port.used == 0, "Image installation and graph preparation execute no bus phases");
  std::uint64_t count = 0;
  constexpr std::array<Byte, 4> inputs{0, 0x7f, 0x80, 0xff};
  constexpr std::array<Byte, 4> flags{0, 1, 0x40, 0xff};
  for (const auto site : source.sites) {
    for (const auto data : inputs) {
      for (const auto ccr : flags) {
        for (unsigned refuse = 0; refuse < 8; ++refuse) {
          for (const unsigned yield : {0U, 1U, 2U, 4U}) {
            one(core, port, site, {.data = data, .ccr = ccr, .refuse = refuse, .yield = yield});
            ++count;
          }
        }
      }
    }
  }
  require(code == source.bytes, "External bus phases cannot overwrite declared code");
  return count;
}
}

void bus_cases(const Source &source) {
  const auto image = compile(source, Backing::external);
  const auto count = mode(source, image, true) + mode(source, image, false);
  std::cout << count << " independent memory-unary bus/fault/yield cases passed.\n";
}
}
