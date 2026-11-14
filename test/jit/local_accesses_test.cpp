// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address entry = 0x4000, local_first = 0x2000;
constexpr std::size_t local_bytes = 8;
constexpr std::uint64_t variant = 7, infinite = std::numeric_limits<std::uint64_t>::max();
constexpr Byte high = 0xa5, low = 0x5a;
constexpr Address word = (Address{high} << 8U) | low;

void require(const bool passed, const char *message) {
  if (!passed) { throw std::runtime_error(message); }
}

struct Bus {
  Core *core = nullptr;
  unsigned reads = 0, writes = 0, masked = 0;
  WordWrite last_write;
  bool stopped = true;
  bool accept_writes = true, accept_reads = true;

  void inspect() noexcept {
    ++reads;
    stopped = stopped && core->foreground_prefix() == 0 && core->event_prefix() == 0 &&
              core->channel_prefix() == 0;
    bool refused = false;
    try {
      core->isolate_accesses({});
    } catch (const std::logic_error &) { refused = true; }
    stopped = stopped && refused;
  }

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    bus.inspect();
    value = (address & 1U) == 0 ? high : low;
    return bus.accept_reads;
  }

  static bool read_word(void *owner, [[maybe_unused]] Address address, Address &value) noexcept {
    static_cast<Bus *>(owner)->inspect();
    value = word;
    return static_cast<Bus *>(owner)->accept_reads;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.writes;
    bus.last_write = {.address = access.address, .value = access.value};
    return bus.accept_writes;
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.writes;
    bus.last_write = access;
    return bus.accept_writes;
  }

  static bool read_masked(void *owner, const MaskedByteRead access, Byte &value) noexcept {
    ++static_cast<Bus *>(owner)->masked;
    return read(owner, access.address, value);
  }

  Io callbacks() noexcept {
    return {.owner = this,
            .read = read,
            .write = write,
            .read_word = read_word,
            .write_word = write_word,
            .read_masked = read_masked};
  }
};

struct Fixture {
  std::vector<Byte> code;
  std::array<Ram, 1> regions;
  Bus bus;
  Io io = bus.callbacks();
  Core core;
  NativeImage image;

  explicit Fixture(const std::span<const Byte> operation)
      : code(operation.begin(), operation.end()),
        regions{Ram{.first = entry, .bytes = {}, .writable = false}},
        core(Memory::external(), &io) {
    constexpr unsigned byte_values = 256;
    code.push_back(isa::opcode<isa::Opcode::bra_relative>()[0]);
    code.push_back(static_cast<Byte>(byte_values - operation.size() - 2));
    regions[0].bytes = code;
    core.set_memory(Memory::mapped(regions));
    bus.core = &core;
    const std::array spans{CodeSpan{.address = entry, .words = code}};
    const std::array roots{entry};
    const std::array variants{CodeVariant{.id = variant, .code = spans, .entries = roots}};
    const std::array memory{RegionLayout{.first = entry, .size = code.size(), .writable = false}};
    const std::array layouts{VariantLayout{.id = variant, .memory = memory}};
    image = Core::compile(
        {.variants = variants},
        {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
    core.install(image, variant);
    core.set_registers({.pc = entry, .x = local_first});
  }

  void isolate() {
    const std::array reads{LocalAccess{.first = local_first, .size = local_bytes}};
    core.isolate_accesses(reads);
  }
};

void addresses(const bool word_read) {
  const auto operation =
      word_read ? isa::program(isa::opcode<isa::Opcode::ldd_indexed>(), std::array<Byte, 1>{0})
                : isa::program(isa::opcode<isa::Opcode::ldaa_indexed>(), std::array<Byte, 1>{0});
  Fixture fixture(operation);
  require(fixture.core.channel_prefix() == 0 && fixture.core.event_prefix() == 0,
          "An opaque owner read is never implicitly isolated");
  fixture.isolate();
  const unsigned width = word_read ? 2U : 1U;
  for (unsigned at = 0; at < address_bytes; ++at) {
    fixture.core.set_registers({.pc = entry, .x = static_cast<Address>(at)});
    const auto state = fixture.core.state();
    const auto time = fixture.core.progress();
    const auto code = fixture.code;
    const auto calls = fixture.bus.reads;
    const auto prefix = fixture.core.channel_prefix();
    const bool direct = at >= entry && at + width <= entry + fixture.code.size();
    const bool local = at >= local_first && at + width <= local_first + local_bytes;
    require(prefix == (direct || local ? infinite : 0) &&
                fixture.core.foreground_prefix() == (direct ? infinite : 0) &&
                fixture.core.event_prefix() == (direct ? infinite : 0) &&
                fixture.core.state() == state && fixture.core.progress() == time &&
                fixture.code == code && fixture.bus.reads == calls,
            "Channel admission checks the complete footprint without executing an owner read");
    const auto executed = fixture.core.run(1);
    require(executed.fault == Fault::none && executed.retired == 1 &&
                fixture.bus.reads == calls + (direct ? 0U : 1U) && fixture.bus.stopped &&
                (direct || fixture.core.registers().d ==
                               (word_read ? word : Address{(at & 1U) == 0 ? high : low} << 8U)),
            "Native reads retain actual callbacks and values both inside and outside isolation");
  }
}

void lifetime() {
  const auto load = isa::program(isa::opcode<isa::Opcode::ldaa_indexed>(), std::array<Byte, 1>{0});
  Fixture fixture(load);
  fixture.isolate();
  constexpr unsigned iterations = 4096;
  constexpr std::uint64_t period = 6;
  require(fixture.core.channel_prefix() == infinite &&
              fixture.core.run_until(iterations * period).retired == iterations * 2 &&
              fixture.bus.reads == iterations && fixture.core.registers().pc == entry &&
              fixture.core.progress().estimated_cycles == iterations * period,
          "One bound local-read description admits thousands of real stateful callbacks");
  for (const auto invalid :
       {LocalAccess{.first = local_first, .size = 0},
        LocalAccess{.first = 1, .size = address_bytes},
        LocalAccess{.first = local_first,
                    .size = 1,
                    .role = std::bit_cast<LocalAccess::Role>(std::numeric_limits<Byte>::max())}}) {
    bool refused = false;
    try {
      fixture.core.isolate_accesses(std::array{invalid});
    } catch (const std::invalid_argument &) { refused = true; }
    require(refused && fixture.core.channel_prefix() == infinite,
            "Invalid read extents fail without replacing the installed isolation proof");
  }
  fixture.core.isolate_accesses({});
  require(fixture.core.channel_prefix() == 0, "Explicit empty isolation revokes its proof");
  fixture.isolate();
  fixture.core.install(fixture.image, variant);
  require(fixture.core.channel_prefix() == 0, "Image installation revokes isolated effects");
  fixture.isolate();
  fixture.core.set_memory(Memory::mapped(fixture.regions));
  require(fixture.core.channel_prefix() == 0, "An explicit remap revokes isolated effects");
}

void writes() {
  const auto store = isa::program(isa::opcode<isa::Opcode::staa_indexed>(), std::array<Byte, 1>{0});
  Fixture fixture(store);
  fixture.isolate();
  require(fixture.core.channel_prefix() == 0 && fixture.core.run(1).retired == 1 &&
              fixture.bus.writes == 1,
          "A read-isolated range never certifies an actual owner write");
}

void masked() {
  const auto test = isa::program(isa::opcode<isa::Opcode::brset_extended>(),
                                 std::array<Byte, 4>{0x20, 0, high, 0});
  Fixture fixture(test);
  fixture.isolate();
  require(fixture.core.channel_prefix() == infinite && fixture.core.event_prefix() == 0 &&
              fixture.bus.reads == 0 && fixture.core.run(1).retired == 1 &&
              fixture.bus.masked == 1 && fixture.bus.reads == 1 && fixture.bus.stopped,
          "Isolated bit tests retain their actual masked-read callback and both CFG paths");
}

void stores(const bool word_store) {
  const auto operation =
      word_store ? isa::program(isa::opcode<isa::Opcode::std_indexed>(), std::array<Byte, 1>{0})
                 : isa::program(isa::opcode<isa::Opcode::staa_indexed>(), std::array<Byte, 1>{0});
  Fixture fixture(operation);
  fixture.core.isolate_accesses(std::array{
      LocalAccess{.first = local_first, .size = local_bytes, .role = LocalAccess::Role::write}});
  const unsigned width = word_store ? 2U : 1U;
  for (unsigned at = 0; at < address_bytes; ++at) {
    fixture.core.set_registers({.pc = entry, .d = word, .x = static_cast<Address>(at)});
    const auto before = fixture.core.state();
    const auto time = fixture.core.progress();
    const auto calls = fixture.bus.writes;
    const bool local = at >= local_first && at + width <= local_first + local_bytes;
    require(fixture.core.channel_prefix() == (local ? 2U : 0U) &&
                fixture.core.foreground_prefix() == 0 && fixture.core.event_prefix() == 0 &&
                fixture.core.state() == before && fixture.core.progress() == time &&
                fixture.bus.writes == calls && fixture.bus.reads == 0,
            "Isolated write admission ends at the first complete byte/word instruction");
    const auto result = fixture.core.run(1);
    require(result.fault == Fault::none && result.retired == 1 &&
                fixture.core.progress().estimated_cycles == time.estimated_cycles + 2 &&
                fixture.bus.writes == calls + 1 && fixture.bus.last_write.address == at &&
                fixture.bus.last_write.value == (word_store ? word : high),
            "Every write footprint preserves the actual callback, value and nominal time");
  }
  constexpr unsigned byte_values = 256;
  for (unsigned value = 0; value < byte_values; ++value) {
    const auto data = static_cast<Address>((value << 8U) | (byte_values - 1 - value));
    fixture.core.set_registers({.pc = entry, .d = data, .x = local_first});
    const auto now = fixture.core.progress().estimated_cycles;
    require(fixture.core.channel_prefix() == 2 && fixture.core.run_until(now + 2).retired == 1 &&
                fixture.core.registers().pc == entry + operation.size() &&
                fixture.bus.last_write.value == (word_store ? data : value),
            "All byte values and paired word patterns stop before the next branch");
    require(fixture.core.channel_prefix() == 5 && fixture.core.run_until(now + 5).retired == 1 &&
                fixture.core.registers().pc == entry && fixture.core.channel_prefix() == 2,
            "The finite local-write boundary propagates through an actual native loop");
  }
}

void exact_writes(const isa::Opcode opcode) {
  constexpr Address direct = 0x80;
  constexpr unsigned byte_values = 256;
  constexpr unsigned byte_bits = 8;
  const bool extended =
      opcode == isa::Opcode::staa_extended || opcode == isa::Opcode::stab_extended;
  const bool accumulator_b =
      opcode == isa::Opcode::stab_direct || opcode == isa::Opcode::stab_extended;
  const auto address = extended ? local_first : direct;
  const auto cycles = extended ? 3U : 2U;
  std::vector<Byte> operation{static_cast<Byte>(opcode)};
  if (extended) { operation.push_back(static_cast<Byte>(address >> byte_bits)); }
  operation.push_back(static_cast<Byte>(address));
  Fixture fixture(operation);
  for (unsigned value = 0; value < byte_values; ++value) {
    const auto other = byte_values - 1 - value;
    const auto data = static_cast<Address>(accumulator_b ? (other << byte_bits) | value
                                                         : (value << byte_bits) | other);
    fixture.core.set_registers({.pc = entry, .d = data});
    const auto before = fixture.core.state();
    const auto time = fixture.core.progress();
    const auto calls = fixture.bus.writes;
    const std::array exact{ByteWrite{.address = address, .value = static_cast<Byte>(value)}};
    const std::array wrong_value{ByteWrite{.address = address, .value = static_cast<Byte>(other)}};
    const std::array wrong_address{
        ByteWrite{.address = static_cast<Address>(address + 1), .value = static_cast<Byte>(value)}};
    const std::array undeclared{InterruptRequest{.vector = 0xfff4, .kind = InterruptKind::xirq}};
    require(fixture.core.channel_prefix(PrefixScope::foreground, {.ranges = {}, .writes = exact}) ==
                    cycles &&
                fixture.core.channel_prefix(PrefixScope::events, {.ranges = {}, .writes = exact}) ==
                    cycles &&
                fixture.core.channel_prefix(std::span<const InterruptRequest>{},
                                            {.ranges = {}, .writes = exact}) == cycles &&
                fixture.core.channel_prefix(undeclared, {.ranges = {}, .writes = exact}) == 0 &&
                fixture.core.channel_prefix(PrefixScope::events,
                                            {.ranges = {}, .writes = wrong_value}) == 0 &&
                fixture.core.channel_prefix(PrefixScope::events,
                                            {.ranges = {}, .writes = wrong_address}) == 0 &&
                fixture.core.channel_prefix() == 0 && fixture.core.foreground_prefix() == 0 &&
                fixture.core.event_prefix() == 0 && fixture.core.state() == before &&
                fixture.core.progress() == time && fixture.bus.writes == calls &&
                fixture.bus.reads == 0,
            "Exact stopped write admission is value/address scoped, read-only and not retained");
    fixture.bus.accept_writes = value != byte_values - 1;
    const auto result = fixture.core.run_until(time.estimated_cycles + cycles);
    const auto accepted = fixture.bus.accept_writes;
    require(result.fault == (accepted ? Fault::none : Fault::memory_write) &&
                result.retired == unsigned{accepted} && fixture.bus.writes == calls + 1 &&
                fixture.bus.last_write.address == address &&
                fixture.bus.last_write.value == value &&
                fixture.core.progress().estimated_cycles ==
                    time.estimated_cycles + (accepted ? cycles : 0) &&
                fixture.core.registers().pc == entry + (accepted ? operation.size() : 0) &&
                fixture.core.registers().d == data &&
                (!accepted ||
                 fixture.core.registers().ccr ==
                     ((unsigned{value == 0} * 4U) | (unsigned{value >= byte_values / 2} * 8U))),
            "Certified writes retain the actual native callback, flags, fault and timing");
  }
}

void nonexact_writes() {
  constexpr auto indexed =
      isa::program(isa::opcode<isa::Opcode::staa_indexed>(), std::array<Byte, 1>{0});
  constexpr auto word_store =
      isa::program(isa::opcode<isa::Opcode::std_extended>(), std::array<Byte, 2>{0x20, 0});
  constexpr auto read_modify_write =
      isa::program(isa::opcode<isa::Opcode::inc_extended>(), std::array<Byte, 2>{0x20, 0});
  for (const auto operation : {std::span<const Byte>(indexed),
                               std::span<const Byte>(word_store),
                               std::span<const Byte>(read_modify_write)}) {
    const Fixture fixture(operation);
    const std::array exact{ByteWrite{.address = local_first, .value = 0}};
    require(fixture.core.channel_prefix(PrefixScope::events, {.ranges = {}, .writes = exact}) == 0,
            "An exact byte write does not certify indexed, word or read-modify-write effects");
  }
}

struct AccessCase {
  std::span<const Byte> code;
  unsigned width, cycles;
  bool read, write;
  Address stored;
};

void transient_range_queries(Fixture &fixture, const AccessCase test) {
  for (const auto role :
       {LocalAccess::Role::read, LocalAccess::Role::write, LocalAccess::Role::read_write}) {
    for (const unsigned extent : {0U, 1U, 2U}) {
      fixture.core.set_registers({.pc = entry, .d = word, .x = word});
      const auto before = fixture.core.state();
      const auto time = fixture.core.progress();
      const std::array range{LocalAccess{.first = local_first, .size = extent, .role = role}};
      const bool permitted =
          extent >= test.width && (role == LocalAccess::Role::read_write ||
                                   (role == LocalAccess::Role::read && !test.write) ||
                                   (role == LocalAccess::Role::write && !test.read));
      require(
          fixture.core.channel_prefix(PrefixScope::foreground, {.ranges = range, .writes = {}}) ==
                  (permitted ? test.cycles : 0) &&
              fixture.core.channel_prefix(PrefixScope::events, {.ranges = range, .writes = {}}) ==
                  (permitted ? test.cycles : 0) &&
              fixture.core.channel_prefix() == 0 && fixture.core.foreground_prefix() == 0 &&
              fixture.core.event_prefix() == 0 && fixture.core.state() == before &&
              fixture.core.progress() == time && fixture.bus.reads == 0 && fixture.bus.writes == 0,
          "Query-local range admission requires the complete read/write footprint without effects");
    }
  }
  for (const auto invalid : {LocalAccess{.first = local_first, .size = address_bytes},
                             LocalAccess{.first = local_first + 1, .size = 2},
                             LocalAccess{.first = local_first,
                                         .size = 2,
                                         .role = std::bit_cast<LocalAccess::Role>(Byte{0xff})}}) {
    const std::array ranges{invalid};
    require(
        fixture.core.channel_prefix(PrefixScope::events, {.ranges = ranges, .writes = {}}) == 0,
        "Invalid, displaced and out-of-address-space transient ranges cannot certify an access");
  }
}

void transient_range_execution(Fixture &fixture, const AccessCase test) {
  for (const auto fault : {Fault::none, Fault::memory_read, Fault::memory_write}) {
    const bool read_failed = fault == Fault::memory_read;
    const bool write_failed = fault == Fault::memory_write;
    if ((read_failed && !test.read) || (write_failed && !test.write)) { continue; }
    fixture.core.set_registers({.pc = entry, .d = word, .x = word});
    fixture.bus.accept_reads = !read_failed;
    fixture.bus.accept_writes = !write_failed;
    const auto before = fixture.core.state();
    const auto time = fixture.core.progress();
    const auto reads = fixture.bus.reads;
    const auto writes = fixture.bus.writes;
    const std::array range{LocalAccess{
        .first = local_first, .size = test.width, .role = LocalAccess::Role::read_write}};
    require(fixture.core.channel_prefix(PrefixScope::foreground, {.ranges = range, .writes = {}}) ==
                test.cycles,
            "A stopped complete range ends at this exact native operation");
    const auto result = fixture.core.run_until(time.estimated_cycles + test.cycles);
    require(result.fault == fault && result.retired == unsigned{fault == Fault::none} &&
                fixture.core.progress().estimated_cycles ==
                    time.estimated_cycles + (fault == Fault::none ? test.cycles : 0) &&
                fixture.bus.reads == reads + unsigned{test.read} &&
                fixture.bus.writes == writes + unsigned{test.write && !read_failed} &&
                (!test.write || read_failed || fixture.bus.last_write.value == test.stored) &&
                (fault == Fault::none || (fixture.core.registers() == before.registers &&
                                          result.pc == entry && result.address == local_first)),
            std::format("Transient opcode=${:02x} expected-fault={} fault={} retired={} "
                        "cycles={}/{} reads={}/{} writes={}/{} value={}/{} pc=${:04x} "
                        "address=${:04x} regs={} width={}",
                        test.code.front(),
                        static_cast<unsigned>(fault),
                        static_cast<unsigned>(result.fault),
                        result.retired,
                        fixture.core.progress().estimated_cycles,
                        time.estimated_cycles,
                        fixture.bus.reads,
                        reads,
                        fixture.bus.writes,
                        writes,
                        fixture.bus.last_write.value,
                        test.stored,
                        result.pc,
                        result.address,
                        fixture.core.registers() == before.registers,
                        test.width)
                .c_str());
  }
}

void range_operations() {
  constexpr auto load =
      isa::program(isa::opcode<isa::Opcode::ldaa_extended>(), std::array<Byte, 2>{0x20, 0});
  constexpr auto load_word =
      isa::program(isa::opcode<isa::Opcode::ldd_extended>(), std::array<Byte, 2>{0x20, 0});
  constexpr auto store =
      isa::program(isa::opcode<isa::Opcode::staa_extended>(), std::array<Byte, 2>{0x20, 0});
  constexpr auto store_word =
      isa::program(isa::opcode<isa::Opcode::std_extended>(), std::array<Byte, 2>{0x20, 0});
  constexpr auto increment =
      isa::program(isa::opcode<isa::Opcode::inc_extended>(), std::array<Byte, 2>{0x20, 0});
  for (const auto test :
       {AccessCase{
            .code = load, .width = 1, .cycles = 3, .read = true, .write = false, .stored = 0},
        AccessCase{
            .code = load_word, .width = 2, .cycles = 3, .read = true, .write = false, .stored = 0},
        AccessCase{
            .code = store, .width = 1, .cycles = 3, .read = false, .write = true, .stored = high},
        AccessCase{.code = store_word,
                   .width = 2,
                   .cycles = 3,
                   .read = false,
                   .write = true,
                   .stored = word},
        AccessCase{.code = increment,
                   .width = 1,
                   .cycles = 4,
                   .read = true,
                   .write = true,
                   .stored = high + 1}}) {
    Fixture fixture(test.code);
    transient_range_queries(fixture, test);
    transient_range_execution(fixture, test);
  }
}

void read_write() {
  const auto operation =
      isa::program(isa::opcode<isa::Opcode::inc_indexed>(), std::array<Byte, 1>{0});
  Fixture fixture(operation);
  for (const auto role : {LocalAccess::Role::read, LocalAccess::Role::write}) {
    fixture.core.isolate_accesses(
        std::array{LocalAccess{.first = local_first, .size = local_bytes, .role = role}});
    require(fixture.core.channel_prefix() == 0,
            "A read-modify-write needs a complete isolated contract for both effects");
  }
  fixture.core.isolate_accesses(std::array{LocalAccess{
      .first = local_first, .size = local_bytes, .role = LocalAccess::Role::read_write}});
  const auto before = fixture.core.state();
  require(fixture.core.channel_prefix() == 3 && fixture.core.state() == before &&
              fixture.bus.reads == 0 && fixture.bus.writes == 0,
          "A complete isolated read-modify-write proof invokes neither callback");
  const auto result = fixture.core.run_until(3);
  require(result.retired == 1 && result.fault == Fault::none && fixture.bus.reads == 1 &&
              fixture.bus.writes == 1 && fixture.bus.last_write.value == high + 1U &&
              fixture.core.registers().pc == entry + operation.size(),
          "The native read and write both occur once before their real instruction boundary");
}
}

int main() try {
  addresses(false);
  addresses(true);
  lifetime();
  writes();
  masked();
  stores(false);
  stores(true);
  read_write();
  for (const auto opcode : {isa::Opcode::staa_direct,
                            isa::Opcode::stab_direct,
                            isa::Opcode::staa_extended,
                            isa::Opcode::stab_extended}) {
    exact_writes(opcode);
  }
  nonexact_writes();
  range_operations();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
