// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "move_model.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace cpu12::test::move {
namespace {
constexpr std::uint16_t ram_first = 0x2000;
constexpr unsigned ram_size = 0x100;

struct Event {
  bool write;
  unsigned width;
  std::uint16_t address, data;
};

struct Port {
  ObservedCore *core = nullptr;
  Registers before{};
  std::array<Byte, address_bytes> ram{};
  std::array<Event, 2> events{};
  std::uint16_t source = 0, destination = 0;
  unsigned used = 0;
  bool read = true, write = true, original = true;

  void observe(const Event event) noexcept {
    original = original && core->registers() == before && used < events.size();
    if (used < events.size()) { events[used] = event; }
    ++used;
  }

  static void clobber() noexcept {
#if defined(__x86_64__)
    asm volatile("movabs $0xabcdef1234567890, %%r9\n\txor %%ecx, %%ecx\n\tstc"
                 :
                 :
                 : "r9", "rcx", "cc");
#endif
  }

  static bool read_byte(void *owner, const std::uint16_t address, Byte &data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    if (address != port.source) { return false; }
    data = port.ram[address];
    port.observe({.write = false, .width = 1, .address = address, .data = data});
    clobber();
    return port.read;
  }

  static bool read_word(void *owner, const std::uint16_t address, std::uint16_t &data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    if (address != port.source) { return false; }
    const auto next = static_cast<std::uint16_t>(address + 1U);
    data = static_cast<std::uint16_t>((static_cast<unsigned>(port.ram[address]) << byte_bits) |
                                      port.ram[next]);
    port.observe({.write = false, .width = 2, .address = address, .data = data});
    clobber();
    return port.read;
  }

  static bool write_byte(void *owner, const ByteWrite data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    if (data.address != port.destination) { return false; }
    port.ram[data.address] = data.value;
    port.observe({.write = true, .width = 1, .address = data.address, .data = data.value});
    clobber();
    return port.write;
  }

  static bool write_word(void *owner, const WordWrite data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    if (data.address != port.destination) { return false; }
    port.ram[data.address] = static_cast<Byte>(data.value >> byte_bits);
    if (port.write) {
      port.ram[static_cast<std::uint16_t>(data.address + 1U)] = static_cast<Byte>(data.value);
    }
    port.observe({.write = true, .width = 2, .address = data.address, .data = data.value});
    clobber();
    return port.write;
  }
};

struct Acceptance {
  bool read = true, write = true, partial = false;
};

bool known(const std::uint16_t address, const bool partial, const unsigned width) {
  if (!partial) { return false; }
  for (unsigned i = 0; i < width; ++i) {
    const auto cell = static_cast<std::uint16_t>(address + i);
    if (cell < ram_first || cell >= ram_first + ram_size) { return false; }
  }
  return true;
}

struct Contract {
  bool read, write, failed_read, failed_write;
};

void fault(ObservedCore &core,
           const Point point,
           const Plan &wanted,
           const Contract access,
           const Port &port) {
  core.set_registers(point.before);
  const auto result = core.run_instruction(wanted.code.view());
  const auto kind = access.failed_read ? Fault::memory_read : Fault::memory_write;
  const auto address = access.failed_read ? wanted.source : wanted.destination;
  const unsigned width = point.word ? 2 : 1;
  require(result.retired == 0 && result.fault == kind && result.pc == point.before.pc &&
              result.opcode == wanted.code.bytes[0] && result.address == address &&
              result.value == point.data && result.access_size == width &&
              core.registers() == point.before,
          "MOV refused source or destination retains exact context and no speculative index/CPU "
          "changes");
  const auto used = port.used;
  const auto stats = core.statistics();
  require(core.run_instruction(wanted.code.view()).fault == kind && port.used == used &&
              core.statistics().read_callbacks == stats.read_callbacks &&
              core.statistics().write_callbacks == stats.write_callbacks &&
              core.registers() == point.before,
          "Latched MOV faults never repeat real source reads or partial destination writes");
}

void one(const Point point, const Acceptance accept) {
  Port port{.before = point.before, .read = accept.read, .write = accept.write};
  const auto wanted = plan(point);
  port.source = wanted.source;
  port.destination = wanted.destination;
  constexpr Byte marker = 0x5a;
  const auto low_destination = static_cast<std::uint16_t>(wanted.destination + 1U);
  port.ram[wanted.destination] = marker;
  port.ram[low_destination] = marker;
  if (point.source.mode != Mode::immediate) {
    port.ram[wanted.source] = static_cast<Byte>(point.word ? point.data >> byte_bits : point.data);
    if (point.word) {
      port.ram[static_cast<std::uint16_t>(wanted.source + 1U)] = static_cast<Byte>(point.data);
    }
  }
  const auto first_before = port.ram[wanted.destination];
  const auto second_before = port.ram[low_destination];
  const unsigned width = point.word ? 2 : 1;
  const bool read =
      point.source.mode != Mode::immediate && !known(wanted.source, accept.partial, width);
  const bool write = !known(wanted.destination, accept.partial, width);
  const Contract access{.read = read,
                        .write = write,
                        .failed_read = read && !accept.read,
                        .failed_write = write && !accept.write};
  Io io{.owner = &port,
        .read = Port::read_byte,
        .write = Port::write_byte,
        .read_word = Port::read_word,
        .write_word = Port::write_word};
  const auto memory =
      accept.partial ? Memory::window({.first = ram_first,
                                       .bytes = std::span(port.ram).subspan(ram_first, ram_size)})
                     : Memory::external();
  ObservedCore core(memory, &io);
  port.core = &core;
  if (access.failed_read || access.failed_write) {
    fault(core, point, wanted, access, port);
  } else {
    retired(core, point, wanted);
  }
  const auto reads = static_cast<unsigned>(read);
  const bool wrote = !access.failed_read;
  const auto writes = static_cast<unsigned>(wrote && write);
  require(port.used == reads + writes && port.original &&
              core.statistics().read_callbacks == reads &&
              core.statistics().write_callbacks == writes,
          "MOV uses exactly the actual logical byte/word source and destination callbacks, "
          "preserving original state until retirement");
  if (read) {
    require(!port.events[0].write && port.events[0].address == wanted.source &&
                port.events[0].width == width && port.events[0].data == point.data,
            "MOV source callback precedes destination and captures the original full value");
  }
  if (writes != 0) {
    const auto event = port.events[reads];
    require(event.write && event.address == wanted.destination && event.width == width &&
                event.data == point.data,
            "MOV destination sees the sequentially resolved address and captured source");
  }
  const auto first_data = static_cast<Byte>(point.word ? point.data >> byte_bits : point.data);
  require(port.ram[wanted.destination] == (wrote ? first_data : first_before),
          "MOV retains an actual first-byte effect even on refused word writes");
  require(port.ram[low_destination] == (point.word && wrote && !access.failed_write
                                            ? static_cast<Byte>(point.data)
                                            : second_before),
          "MOV preserves unperformed low-word writes and never invents rollback or completion");
}

struct Layout {
  bool partial;
  std::uint16_t source, destination;
};

void phases(const Point point, const bool partial) {
  for (const bool read : {false, true}) {
    for (const bool write : {false, true}) {
      one(point, {.read = read, .write = write, .partial = partial});
    }
  }
}

void layout_cases(const Layout layout) {
  auto before = seed;
  before.x = layout.source;
  before.y = layout.destination;
  const Operand source{.mode = Mode::extended, .address = layout.source};
  const Operand destination{.mode = Mode::extended, .address = layout.destination};
  const Operand indexed_source{.mode = Mode::indexed};
  const Operand indexed_destination{.mode = Mode::indexed,
                                    .index = {.base = &Registers::y, .postbyte = 0x40}};
  constexpr std::array<unsigned, 8> values{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xa55a, 0xffff};
  for (const bool word : {false, true}) {
    for (const auto from : {Operand{}, source, indexed_source}) {
      for (const auto to : {destination, indexed_destination}) {
        for (const auto data : values) {
          phases({.word = word,
                  .source = from,
                  .destination = to,
                  .before = before,
                  .data = word ? data : data & byte_mask},
                 layout.partial);
        }
      }
    }
  }
}

void automatic_cases() {
  for (const auto source : indexes()) {
    if (source.update == 0) { continue; }
    const Byte base = static_cast<Byte>(source.postbyte & 0xc0U);
    const std::array<Index, 3> destinations{
        {{.base = source.base, .postbyte = base},
         {.base = source.base, .update = 1, .postbyte = static_cast<Byte>(base | 0x30U)},
         {.base = source.base,
          .accumulator = Offset::a,
          .postbyte =
              static_cast<Byte>(0xe4U + (static_cast<unsigned>(source.postbyte >> 6U) << 3U))}}};
    constexpr unsigned data = 0x1234;
    for (const auto destination : destinations) {
      for (const bool word : {false, true}) {
        phases({.word = word,
                .source = {.mode = Mode::indexed, .index = source},
                .destination = {.mode = Mode::indexed, .index = destination},
                .before = seed,
                .data = word ? data : data & byte_mask},
               false);
      }
    }
  }
}
}

void external_cases() {
  constexpr std::array<Layout, 7> layouts{
      {{.partial = false, .source = 0xffff, .destination = 0},
       {.partial = false, .source = 0xffff, .destination = 0xffff},
       {.partial = true, .source = 0x2000, .destination = 0x4000},
       {.partial = true, .source = 0x4000, .destination = 0x2000},
       {.partial = true, .source = 0x20ff, .destination = 0x4000},
       {.partial = true, .source = 0x4000, .destination = 0x20ff},
       {.partial = true, .source = 0x2000, .destination = 0x2004}}};
  for (const auto layout : layouts) { layout_cases(layout); }
  automatic_cases();
}
}
