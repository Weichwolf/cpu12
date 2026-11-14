// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
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
  bool stopped = true;

  void inspect() noexcept {
    ++reads;
    stopped = stopped && core->foreground_prefix() == 0 && core->event_prefix() == 0 &&
              core->channel_prefix() == 0;
    bool refused = false;
    try {
      core->isolate_reads({});
    } catch (const std::logic_error &) { refused = true; }
    stopped = stopped && refused;
  }

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    bus.inspect();
    value = (address & 1U) == 0 ? high : low;
    return true;
  }

  static bool read_word(void *owner, [[maybe_unused]] Address address, Address &value) noexcept {
    static_cast<Bus *>(owner)->inspect();
    value = word;
    return true;
  }

  static bool write(void *owner, [[maybe_unused]] ByteWrite access) noexcept {
    ++static_cast<Bus *>(owner)->writes;
    return true;
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
    const std::array reads{LocalRead{.first = local_first, .size = local_bytes}};
    core.isolate_reads(reads);
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
       {LocalRead{.first = local_first, .size = 0}, LocalRead{.first = 1, .size = address_bytes}}) {
    bool refused = false;
    try {
      fixture.core.isolate_reads(std::array{invalid});
    } catch (const std::invalid_argument &) { refused = true; }
    require(refused && fixture.core.channel_prefix() == infinite,
            "Invalid read extents fail without replacing the installed isolation proof");
  }
  fixture.core.isolate_reads({});
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
}

int main() try {
  addresses(false);
  addresses(true);
  lifetime();
  writes();
  masked();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
