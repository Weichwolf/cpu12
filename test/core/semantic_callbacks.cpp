// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "public_io.hpp"
#include "semantic_consumer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace cpu12::test::semantic {
namespace {
constexpr Address entry = 0x8000, input = 0x1234, output = 0x2345;
constexpr Byte mask = 0x80, low_mask = 0xa5, backedge = 0xfe;
constexpr Registers seed{
    .pc = entry, .d = 0xabcd, .x = 0x3456, .y = 0x789a, .sp = 0x4321, .ccr = 0xa5};
constexpr unsigned operations = 5, byte_bits = 8, byte_mask = 255;
constexpr std::array callbacks{
    CallbackBinding{.role = CallbackRole::read, .symbol = "fixture_read"},
    CallbackBinding{.role = CallbackRole::write, .symbol = "fixture_write"},
    CallbackBinding{.role = CallbackRole::read_word, .symbol = "fixture_read_word"},
    CallbackBinding{.role = CallbackRole::write_word, .symbol = "fixture_write_word"},
    CallbackBinding{.role = CallbackRole::read_masked, .symbol = "fixture_read_masked"}};
constexpr auto program = isa::program(isa::opcode<isa::Opcode::ldaa_extended>(),
                                      std::array<Byte, 2>{0x12, 0x34},
                                      isa::opcode<isa::Opcode::staa_extended>(),
                                      std::array<Byte, 2>{0x23, 0x45},
                                      isa::opcode<isa::Opcode::ldd_extended>(),
                                      std::array<Byte, 2>{0x12, 0x34},
                                      isa::opcode<isa::Opcode::std_extended>(),
                                      std::array<Byte, 2>{0x23, 0x45},
                                      isa::opcode<isa::Opcode::brset_extended>(),
                                      std::array<Byte, 4>{0x12, 0x34, mask, 0},
                                      isa::opcode<isa::Opcode::bra_relative>(),
                                      std::array{backedge});
constexpr Address idle = entry + program.size() - 2;

void require(const bool value, const char *message) {
  if (!value) { throw std::runtime_error(message); }
}

NativeImage prepare(const MemoryTopology topology, const std::span<const std::byte> host) {
  const std::array code{CodeSpan{.address = entry, .words = program}};
  constexpr std::array entries{entry};
  const std::array variants{CodeVariant{.id = 1, .code = code, .entries = entries}};
  constexpr std::array regions{
      RegionLayout{.first = entry, .size = program.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = 1, .memory = regions}};
  return Core::compile({.variants = variants},
                       {.memory = topology == MemoryTopology::fixed
                                      ? std::span<const VariantLayout>(layouts)
                                      : std::span<const VariantLayout>{},
                        .interrupts = {},
                        .transitions = {},
                        .callbacks = callbacks,
                        .bindings = host,
                        .topology = topology});
}

struct Snapshot {
  RunResult result;
  State state;
  Progress progress;
  std::array<public_io::Access, 16> trace;
  std::array<Byte, 2> written;
  unsigned calls;
  bool operator==(const Snapshot &) const = default;
};

struct Point {
  Byte value;
  unsigned failure;
  bool yielding = false;
};

void verify_trace(const public_io::Bus &bus) {
  constexpr std::array<Byte, operations> widths{1, 1, 2, 2, 1};
  for (unsigned index = 0; index < bus.calls; ++index) {
    const auto &access = bus.trace[index];
    const bool write = index == 1 || index == 3;
    require(access.width == widths[index] && access.write == write &&
                access.address == (write ? output : input) &&
                access.mask == (index == 4 ? mask : byte_mask) &&
                access.state.registers.pc == entry + (index * 3) &&
                access.progress.estimated_cycles == std::uint64_t{index} * 3,
            "Every host observation sees the exact prior CPU state and nominal time");
  }
}

void verify_fault(const RunResult result, const Point point) {
  const auto value = point.value;
  const auto failure = point.failure;
  const bool write = failure == 1 || failure == 3;
  const bool word = failure == 2 || failure == 3;
  const auto datum =
      word ? static_cast<Address>((unsigned{value} << byte_bits) | (value ^ low_mask)) : value;
  constexpr std::array<Byte, operations> masks{byte_mask, 0, 0, 0, mask};
  require(result.fault == (write ? Fault::memory_write : Fault::memory_read) &&
              result.address == (write ? output : input) && result.value == datum &&
              result.access_size == (word ? 2 : 1) && result.read_mask == masks[failure],
          "A failing bound callback retains actual address, operation, value, width and mask");
}

void verify_prefix(const Core &core,
                   const public_io::Bus &bus,
                   const RunResult result,
                   const Point point) {
  const auto [value, failure, yielding] = point;
  const bool failed = failure < operations;
  const unsigned retired = failed ? failure : operations;
  const auto expected_cycles = (std::uint64_t{retired} * 3) + (retired == operations ? 2 : 0);
  require(
      bus.calls == (failed ? failure + 1 : operations) &&
          core.progress().estimated_cycles == expected_cycles &&
          result.retired == (yielding ? 1 : retired) &&
          core.registers().pc == (failed ? entry + (failure * 3) : idle) &&
          core.statistics().compiled_entries == 0,
      "Each selected callback preserves its exact completed prefix, PC, cycles and no translation");
  if (failed) {
    verify_fault(result, point);
  } else {
    require(result.fault == Fault::none &&
                core.registers().d ==
                    static_cast<Address>((unsigned{value} << byte_bits) | (value ^ low_mask)),
            "Byte, word and masked bindings preserve original successful values");
  }
}

Snapshot io_prefix(const NativeImage &image, const Point point) {
  const auto [value, failure, yielding] = point;
  auto source = program;
  public_io::Bus bus;
  bus.memory[input] = value;
  bus.memory[input + 1] = value ^ low_mask;
  bus.fail_at = failure;
  bus.yield = yielding;
  Io io{.owner = &bus};
  Core core(Memory::window({.first = entry, .bytes = source, .writable = false}), &io);
  bus.core = &core;
  core.set_registers(seed);
  core.install(image, 1);
  auto result = core.run(operations);
  if (yielding && result.fault == Fault::none) {
    for (unsigned index = 1; index < operations; ++index) {
      require(result.retired == 1,
              "Every fused callback yields after exactly one completed instruction");
      result = core.run(operations);
    }
  }
  verify_prefix(core, bus, result, point);
  verify_trace(bus);
  return {.result = result,
          .state = core.state(),
          .progress = core.progress(),
          .trace = bus.trace,
          .written = {bus.memory[output], bus.memory[output + 1]},
          .calls = bus.calls};
}

void mapped_ram(const NativeImage &image, const Byte value, const bool writable) {
  auto source = program;
  std::array<Byte, 2> data{value, static_cast<Byte>(value ^ low_mask)};
  std::array<Byte, 2> written{};
  std::array<Byte, 2> known{};
  const std::array grants{
      Ram{.first = entry, .bytes = source, .writable = false},
      Ram{.first = input, .bytes = data},
      Ram{.first = output, .bytes = written, .known = known, .writable = writable}};
  public_io::Bus bus;
  Io io{.owner = &bus};
  Core core(Memory::mapped(grants), &io);
  bus.core = &core;
  core.set_registers(seed);
  core.install(image, 1);
  const auto result = core.run(operations);
  const auto word = static_cast<Address>((unsigned{value} << byte_bits) | (value ^ low_mask));
  constexpr std::uint64_t cycles = 17;
  require(result.fault == Fault::none && result.retired == operations &&
              core.registers().pc == idle && core.registers().d == word &&
              core.progress().estimated_cycles == cycles && core.statistics().compiled_entries == 0,
          "Dynamic grants retain original data and cycles with selected host bindings");
  if (writable) {
    require(bus.calls == 0 && written == data && known == std::array<Byte, 2>{1, 1},
            "Initialized direct RAM bypasses every selected I/O binding and publishes real store "
            "knownness");
  } else {
    require(bus.calls == 2 && bus.trace[0].width == 1 && bus.trace[1].width == 2 &&
                bus.memory[output] == data[0] && bus.memory[output + 1] == data[1] &&
                written == std::array<Byte, 2>{} && known == std::array<Byte, 2>{},
            "Read-only RAM delegates whole byte/word writes to selected bindings without changing "
            "the direct grant");
  }
}
}

void callback_roles(const std::span<const std::byte> host) {
  const auto fixed = prepare(MemoryTopology::fixed, host);
  const auto dynamic = prepare(MemoryTopology::dynamic, host);
  for (unsigned value = 0; value <= byte_mask; ++value) {
    for (unsigned failure = 0; failure <= operations; ++failure) {
      const auto expected =
          io_prefix(fixed, {.value = static_cast<Byte>(value), .failure = failure});
      require(expected ==
                  io_prefix(dynamic, {.value = static_cast<Byte>(value), .failure = failure}),
              "Dynamic and fixed binding paths have identical full state, progress, faults and "
              "callback effects");
    }
    require(
        io_prefix(fixed,
                  {.value = static_cast<Byte>(value), .failure = operations, .yielding = true}) ==
            io_prefix(dynamic,
                      {.value = static_cast<Byte>(value), .failure = operations, .yielding = true}),
        "Dynamic callback yields resume exactly once with identical observations");
    mapped_ram(dynamic, static_cast<Byte>(value), true);
    mapped_ram(dynamic, static_cast<Byte>(value), false);
  }
}
}
