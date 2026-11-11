// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "move_model.hpp"
#include "preparation_move.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>

namespace cpu12::test::prepared_move {
namespace {
struct Ram {
  explicit Ram(const Source &source) : last(first + source.bytes.size()) {
    std::ranges::copy(source.bytes, bytes.begin() + first);
  }

  std::array<Byte, address_bytes> bytes{};
  std::array<cpu12::Ram, 3> grants;
  std::size_t last;
  Core *core = nullptr;
  Registers before;
  Progress progress;
  std::uint64_t reads = 0, writes = 0;
  bool original = true;

  [[nodiscard]] bool code(const Address at) const { return at >= first && at < last; }

  [[nodiscard]] unsigned region(const Address at) const {
    if (at < first) { return 0; }
    return code(at) ? 1 : 2;
  }

  void observe() noexcept {
    original = original && core->registers() == before && core->progress() == progress;
  }

  static bool read_word(void *owner, const Address at, Address &value) noexcept {
    auto &ram = *static_cast<Ram *>(owner);
    ++ram.reads;
    ram.observe();
    value = static_cast<Address>((unsigned{ram.bytes[at]} << byte_bits) |
                                 ram.bytes[static_cast<Address>(at + 1)]);
    return true;
  }

  static bool write_byte(void *owner, const ByteWrite access) noexcept {
    auto &ram = *static_cast<Ram *>(owner);
    ++ram.writes;
    ram.observe();
    return !ram.code(access.address);
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    auto &ram = *static_cast<Ram *>(owner);
    ++ram.writes;
    ram.observe();
    const auto next = static_cast<Address>(access.address + 1);
    if (ram.code(access.address) || ram.code(next)) { return false; }
    ram.bytes[access.address] = static_cast<Byte>(access.value >> byte_bits);
    ram.bytes[next] = static_cast<Byte>(access.value);
    return true;
  }

  Io io() {
    return {.owner = this, .write = write_byte, .read_word = read_word, .write_word = write_word};
  }

  Memory memory() {
    grants = {
        cpu12::Ram{.first = first,
                   .bytes = std::span(bytes).subspan(first, last - first),
                   .writable = false},
        cpu12::Ram{.first = 0, .bytes = std::span(bytes).first(first)},
        cpu12::Ram{.first = static_cast<Address>(last), .bytes = std::span(bytes).subspan(last)}};
    return Memory::mapped(grants);
  }

  void put(const Address at, const Byte value) {
    if (!code(at)) { bytes[at] = value; }
  }
};

struct Point {
  unsigned data;
  Byte ccr;
  unsigned deadline = 0;
};

unsigned initialize(Ram &ram, const Site &site, const unsigned data) {
  constexpr Byte high = 0x5a;
  constexpr Byte low = 0xa5;
  ram.put(site.plan.destination, high);
  ram.put(static_cast<Address>(site.plan.destination + 1), low);
  if (site.point.source.mode == move::Mode::immediate) { return site.point.data; }
  ram.put(site.plan.source, static_cast<Byte>(site.point.word ? data >> byte_bits : data));
  if (site.point.word) {
    ram.put(static_cast<Address>(site.plan.source + 1), static_cast<Byte>(data));
  }
  if (!site.point.word) { return ram.bytes[site.plan.source]; }
  return (unsigned{ram.bytes[site.plan.source]} << byte_bits) |
         ram.bytes[static_cast<Address>(site.plan.source + 1)];
}

struct Model {
  State state;
  unsigned count = 0, cycles = 0;
  Address last;
  Byte opcode;
  bool active, accepted;
};

Model predict(const Ram &ram, const Site &site, const Point point, const unsigned value) {
  auto before = site.point.before;
  before.ccr = point.ccr;
  const auto destination = site.plan.destination;
  const bool active = point.deadline != 1;
  const bool accepted = active && !ram.code(destination) &&
                        (!site.point.word || !ram.code(static_cast<Address>(destination + 1)));
  Model model{.state = {.registers = before, .failure = {}},
              .last = before.pc,
              .opcode = active ? site.plan.code.bytes[0] : Byte{0},
              .active = active,
              .accepted = accepted};
  if (!active) { return model; }
  if (!accepted) {
    model.state.failure = {.fault = Fault::memory_write,
                           .pc = before.pc,
                           .opcode = site.plan.code.bytes[0],
                           .address = destination,
                           .value = static_cast<Address>(value),
                           .access_size = static_cast<std::uint8_t>(site.point.word ? 2 : 1)};
    return model;
  }
  const auto cycles = site.cycles();
  const auto delta = point.deadline == 0 ? 0 : point.deadline - 1;
  const auto loops = delta > cycles ? (delta - cycles + 2) / 3 : 0;
  model.count = 1 + loops;
  model.cycles = cycles + (loops * 3);
  model.state.registers = site.plan.wanted;
  model.state.registers.ccr = point.ccr;
  if (loops != 0) {
    model.last = model.state.registers.pc;
    model.opcode = idle[0];
  }
  return model;
}

Byte written(const Byte previous,
             const Site &site,
             const Address at,
             const bool accepted,
             const unsigned value) {
  if (accepted && at == site.plan.destination) {
    return static_cast<Byte>(site.point.word ? value >> byte_bits : value);
  }
  if (accepted && site.point.word && at == static_cast<Address>(site.plan.destination + 1)) {
    return static_cast<Byte>(value);
  }
  return previous;
}

void check(Core &core, Ram &ram, const Site &site, const Point point) {
  const auto value = initialize(ram, site, point.data);
  auto before = site.point.before;
  before.ccr = point.ccr;
  core.set_registers(before);
  const auto progress = core.progress().estimated_cycles;
  ram.before = before;
  ram.progress = core.progress();
  const auto reads = ram.reads;
  const auto writes = ram.writes;
  const auto compiled = core.statistics().compiled_entries;
  const auto source_low = static_cast<Address>(site.plan.source + 1);
  const auto destination = site.plan.destination;
  const auto destination_low = static_cast<Address>(destination + 1);
  const auto width = site.point.word ? 2U : 1U;
  const std::array watched{site.plan.source,
                           source_low,
                           destination,
                           destination_low,
                           static_cast<Address>(destination + word_mask),
                           static_cast<Address>(destination + width)};
  std::array<Byte, watched.size()> memory{};
  for (std::size_t index = 0; index < watched.size(); ++index) {
    memory[index] = ram.bytes[watched[index]];
  }
  const auto model = predict(ram, site, point, value);
  const auto execution =
      point.deadline == 0 ? core.run(1) : core.run_until(progress + point.deadline - 1);
  for (std::size_t index = 0; index < watched.size(); ++index) {
    const auto expected = written(memory[index], site, watched[index], model.accepted, value);
    require(
        ram.bytes[watched[index]] == expected,
        "Prepared MOV captures before overlapping writes and preserves unperformed writes/code");
  }
  const bool source_bus = model.active && site.point.word &&
                          site.point.source.mode != move::Mode::immediate &&
                          ram.region(site.plan.source) != ram.region(source_low);
  const bool destination_bus =
      model.active && (!model.accepted ||
                       (site.point.word && ram.region(destination) != ram.region(destination_low)));
  require(execution.retired == model.count && execution.fault == model.state.failure.fault &&
              core.state() == model.state && execution.pc == model.last &&
              execution.opcode == model.opcode &&
              core.progress().estimated_cycles == progress + model.cycles && ram.original &&
              ram.reads == reads + (source_bus ? 1U : 0U) &&
              ram.writes == writes + (destination_bus ? 1U : 0U) &&
              core.statistics().compiled_entries == compiled,
          "Prepared MOV preserves independent sequential index addresses, full CCR, exact atomic "
          "deadlines and logical word-owner calls");
}

std::uint64_t matrix(Core &core, Ram &ram, const Source &source) {
  std::uint64_t count = 0;
  constexpr std::array<Byte, 4> flags{0, 0xff, 0xa5, 0x5a};
  for (const auto &site : source.sites) {
    if (site.full) {
      const auto limit = site.point.word ? word_mask : byte_mask;
      for (unsigned data = 0; data <= limit; ++data) {
        for (const auto ccr : flags) {
          check(core, ram, site, {.data = data, .ccr = ccr});
          ++count;
        }
      }
    } else {
      for (const auto data : edges) {
        for (const auto ccr : flags) {
          check(core, ram, site, {.data = data, .ccr = ccr});
          ++count;
        }
      }
    }
  }
  return count;
}

std::uint64_t qualify(const Group group) {
  const Source source(group);
  const auto image = compile(source, false);
  Ram ram(source);
  auto io = ram.io();
  Core core(ram.memory(), &io);
  ram.core = &core;
  core.install(image, variant);
  auto count = matrix(core, ram, source);
  Ram second(source);
  auto second_io = second.io();
  Core graph(second.memory(), &second_io);
  second.core = &graph;
  const auto report = graph.prepare(source.entries);
  require(report.failure.fault == Fault::none && report.unresolved_entries == 0 &&
              report.prepared_entries == source.sites.size() * 2 && report.native_bytes != 0,
          "Graph preparation covers every declared MOV site and successor before execution");
  count += matrix(graph, second, source);
  for (const auto &site : source.sites) {
    for (unsigned deadline = 1; deadline <= 13; ++deadline) {
      check(core, ram, site, {.data = sample, .ccr = byte_mask, .deadline = deadline});
      check(graph, second, site, {.data = sample, .ccr = byte_mask, .deadline = deadline});
    }
  }
  require(
      std::ranges::equal(source.bytes, std::span(ram.bytes).subspan(first, source.bytes.size())) &&
          std::ranges::equal(source.bytes,
                             std::span(second.bytes).subspan(first, source.bytes.size())),
      "Every prepared MOV form retains the declared executable bytes");
  return count;
}
}
}

int main(const int argc, const char *const *const argv) {
  try {
    using namespace cpu12::test::prepared_move;
    constexpr std::array<std::string_view, 5> names{"ordinary", "x", "y", "sp", "bus"};
    constexpr std::array groups{Group::ordinary, Group::x, Group::y, Group::sp};
    require(argc == 2, "Choose one independent MOV image group");
    const std::string_view selected(argv[1]);
    require(std::ranges::find(names, selected) != names.end(), "Unknown MOV image group");
    if (selected == names.back()) {
      bus_cases();
      return 0;
    }
    const auto position =
        static_cast<std::size_t>(std::ranges::find(names, selected) - names.begin());
    const auto count = qualify(groups[position]);
    std::cout << "Verified " << count << " independent prepared MOV states and prefix deadlines in "
              << selected << ".\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
