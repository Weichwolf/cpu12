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

namespace {
using namespace cpu12;
constexpr Address entry = 0x4000, sentinel_x = 0xa55a, sentinel_y = 0x4321, stack = 0x9000;
constexpr auto sub = static_cast<Byte>(isa::Opcode::subd_immediate);
constexpr auto bne = static_cast<Byte>(isa::Opcode::bne_relative);
constexpr Byte arithmetic_backedge = 0xfb;
constexpr Byte read_value = 0x5a;
constexpr Address read_word = static_cast<Address>(read_value << 8);
constexpr auto bra = static_cast<Byte>(isa::Opcode::bra_relative);
constexpr std::array<Byte, 7> loop{sub, 0, 1, bne, 0xfb, bra, 0xf9};
constexpr std::array<Address, 1> entries{entry};

void require(bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

void native_grants() {
  auto bytes = loop;
  const auto memory = Memory::window({.first = entry, .bytes = bytes, .writable = false});
  Core prepared(memory);
  Core reference(memory);
  const auto state = prepared.state();
  const auto progress = prepared.progress();
  const auto report = prepared.prepare(entries);
  require(report.failure.fault == Fault::none && report.prepared_entries == 3 &&
              report.unresolved_entries == 0 && report.native_bytes != 0 &&
              prepared.state() == state && prepared.progress() == progress,
          "Preparation must install real optimized entries without executing guest instructions");
  const auto statistics = prepared.statistics();
  const auto reused = prepared.prepare(entries);
  require(reused.prepared_entries == report.prepared_entries && reused.native_bytes == 0 &&
              prepared.statistics().compiled_entries == statistics.compiled_entries,
          "Repeated preparation must reuse retained code without allocating another native image");
  constexpr std::array<Address, 7> values{0, 1, 2, 0x137, 0x7fff, 0x8000, 0xffff};
  constexpr std::array<std::uint32_t, 8> grants{0, 1, 2, 3, 4, 5, 17, 1024};
  constexpr std::array<std::uint64_t, 8> deadlines{0, 1, 2, 3, 4, 5, 17, 64};
  for (const auto value : values) {
    for (unsigned ccr = 0; ccr <= std::numeric_limits<Byte>::max(); ++ccr) {
      for (const auto pc : {entry, Address{entry + 3}, Address{entry + 5}}) {
        const Registers seed{.pc = pc,
                             .d = value,
                             .x = sentinel_x,
                             .y = sentinel_y,
                             .sp = stack,
                             .ccr = static_cast<Byte>(ccr)};
        for (const auto grant : grants) {
          prepared.set_registers(seed);
          reference.set_registers(seed);
          require(
              prepared.run(grant) == reference.run(grant) &&
                  prepared.state() == reference.state() &&
                  prepared.progress() == reference.progress(),
              "Prepared native execution must retain full state/result/cycles for every prefix");
        }
        for (const auto delta : deadlines) {
          prepared.set_registers(seed);
          reference.set_registers(seed);
          const auto deadline = prepared.progress().estimated_cycles + delta;
          require(prepared.run_until(deadline) == reference.run_until(deadline) &&
                      prepared.state() == reference.state() &&
                      prepared.progress() == reference.progress(),
                  "Prepared native execution must preserve atomic cycle-deadline boundaries");
        }
      }
    }
  }
  require(bytes == loop && prepared.statistics().compiled_entries == statistics.compiled_entries,
          "Prepared runs must not mutate source or trigger lazy compilation");
}

void retained_native_boundary() {
  constexpr auto program = isa::program(isa::opcode<isa::Opcode::addd_immediate>(),
                                        std::array<Byte, 2>{0, 1},
                                        isa::opcode<isa::Opcode::ldd_extended>(),
                                        std::array<Byte, 2>{0x20, 0},
                                        isa::opcode<isa::Opcode::subd_immediate>(),
                                        std::array<Byte, 2>{0, 1},
                                        isa::opcode<isa::Opcode::rts>());
  auto bytes = program;
  constexpr Address loaded = 0x7fff;
  std::array<Byte, 2> value{static_cast<Byte>(loaded >> 8U), static_cast<Byte>(loaded)};
  constexpr Address data = 0x2000;
  const std::array windows{Ram{.first = entry, .bytes = bytes, .writable = false},
                           Ram{.first = data, .bytes = value, .writable = false}};
  const auto memory = Memory::mapped(windows);
  Core prepared(memory);
  Core reference(memory);
  const auto report = prepared.prepare(entries);
  require(report.prepared_entries == 2 && report.unresolved_entries == 2 &&
              report.failure.fault == Fault::none,
          "Unmigrated valid ISA and dynamic return edges must remain explicit");
  const Registers seed{.pc = entry, .d = sentinel_x, .x = sentinel_x, .y = sentinel_y, .sp = stack};
  for (const auto grant : {1U, 2U, 3U}) {
    prepared.set_registers(seed);
    reference.set_registers(seed);
    require(prepared.run(grant) == reference.run(grant) && prepared.state() == reference.state() &&
                prepared.progress() == reference.progress(),
            "Prepared and retained native entries must join without losing partial retirement or "
            "effects");
  }
}

void binding_lifetime() {
  auto first = loop;
  auto second = loop;
  second[0] = static_cast<Byte>(isa::Opcode::addd_immediate);
  const auto old_memory =
      Memory::window({.first = entry, .bytes = first, .writable = false, .identity = 1});
  const auto new_memory =
      Memory::window({.first = entry, .bytes = second, .writable = false, .identity = 2});
  Core core(old_memory);
  require(core.prepare(entries).native_bytes != 0,
          "First immutable binding must produce native code");
  core.set_memory(new_memory);
  require(core.prepare(entries).native_bytes != 0,
          "A new binding must prepare its own original bytes");
  const auto compilations = core.statistics().compiled_entries;
  core.set_registers({.pc = entry, .d = 1});
  require(core.run(1).retired == 1 && core.registers().d == 2,
          "New binding must use its own add semantics");
  core.set_memory(old_memory);
  require(core.prepare(entries).native_bytes == 0 &&
              core.statistics().compiled_entries == compilations,
          "Rebinding an unchanged immutable identity must retain its executable lifetime");
  core.set_registers({.pc = entry, .d = 1});
  require(core.run(1).retired == 1 && core.registers().d == 0 && first == loop,
          "Previous binding must retain its original subtract semantics and source bytes");
}

void arithmetic_entries() {
  constexpr std::array operations{isa::Opcode::addd_immediate, isa::Opcode::subd_immediate};
  constexpr std::array<Address, 5> operands{0, 1, 0x7fff, 0x8000, 0xffff};
  constexpr std::array<Address, 7> values{0, 1, 2, 0x7ffe, 0x7fff, 0x8000, 0xffff};
  for (const auto operation : operations) {
    for (const auto operand : operands) {
      std::array<Byte, 5> bytes{static_cast<Byte>(operation),
                                static_cast<Byte>(operand >> 8),
                                static_cast<Byte>(operand),
                                bra,
                                arithmetic_backedge};
      const auto memory = Memory::window({.first = entry, .bytes = bytes, .writable = false});
      Core prepared(memory);
      Core reference(memory);
      require(prepared.prepare(entries).prepared_entries == 2,
              "Immediate arithmetic and its established backedge must be genuinely compiled");
      for (const auto value : values) {
        for (unsigned ccr = 0; ccr <= std::numeric_limits<Byte>::max(); ++ccr) {
          const Registers seed{.pc = entry,
                               .d = value,
                               .x = sentinel_x,
                               .y = sentinel_y,
                               .sp = stack,
                               .ccr = static_cast<Byte>(ccr)};
          prepared.set_registers(seed);
          reference.set_registers(seed);
          require(prepared.run(1) == reference.run(1) && prepared.state() == reference.state() &&
                      prepared.progress() == reference.progress(),
                  "Installed arithmetic must preserve individual carry, overflow, wrap and upper "
                  "flags");
        }
      }
    }
  }
}

void index_prefixes(Core &prepared, Core &reference, const Registers seed) {
  constexpr std::array<std::uint32_t, 7> grants{0, 1, 2, 3, 4, 5, 17};
  constexpr std::array<std::uint64_t, 7> deadlines{0, 1, 2, 3, 4, 5, 17};
  for (const auto pc : {entry, Address{entry + 1}}) {
    auto at_entry = seed;
    at_entry.pc = pc;
    for (const auto grant : grants) {
      prepared.set_registers(at_entry);
      reference.set_registers(at_entry);
      require(prepared.run(grant) == reference.run(grant) &&
                  prepared.state() == reference.state() &&
                  prepared.progress() == reference.progress(),
              "Index lowering must preserve complete prefix state and progress");
    }
    for (const auto delta : deadlines) {
      prepared.set_registers(at_entry);
      reference.set_registers(at_entry);
      const auto deadline = prepared.progress().estimated_cycles + delta;
      require(prepared.run_until(deadline) == reference.run_until(deadline) &&
                  prepared.state() == reference.state() &&
                  prepared.progress() == reference.progress(),
              "Index lowering must preserve atomic cycle deadlines");
    }
  }
}

struct IndexCase {
  isa::Opcode opcode;
  bool x;
  int delta;
};

Registers index_result(Registers seed, const IndexCase operation) {
  auto &value = operation.x ? seed.x : seed.y;
  value = static_cast<Address>(value + operation.delta);
  constexpr unsigned zero = 4;
  seed.ccr = static_cast<Byte>((seed.ccr & (std::numeric_limits<Byte>::max() ^ zero)) |
                               (value == 0 ? zero : 0));
  seed.pc = entry + 1;
  return seed;
}

void index_entries() {
  constexpr std::array operations{IndexCase{.opcode = isa::Opcode::inx, .x = true, .delta = 1},
                                  IndexCase{.opcode = isa::Opcode::dex, .x = true, .delta = -1},
                                  IndexCase{.opcode = isa::Opcode::iny, .x = false, .delta = 1},
                                  IndexCase{.opcode = isa::Opcode::dey, .x = false, .delta = -1}};
  constexpr std::array<Address, 7> values{0, 1, 2, 0x7fff, 0x8000, 0xfffe, 0xffff};
  constexpr Byte backedge = 0xfd;
  for (const auto operation : operations) {
    std::array<Byte, 3> bytes{static_cast<Byte>(operation.opcode), bra, backedge};
    const auto memory = Memory::window({.first = entry, .bytes = bytes, .writable = false});
    Core prepared(memory);
    Core reference(memory);
    const auto report = prepared.prepare(entries);
    require(report.prepared_entries == 2 && report.unresolved_entries == 0 &&
                report.failure.fault == Fault::none && report.native_bytes != 0,
            "Index operations and their backedge must be genuinely compiled");
    for (const auto value : values) {
      for (unsigned ccr = 0; ccr <= std::numeric_limits<Byte>::max(); ++ccr) {
        Registers seed{.pc = entry,
                       .d = sentinel_x,
                       .x = sentinel_x,
                       .y = sentinel_y,
                       .sp = stack,
                       .ccr = static_cast<Byte>(ccr)};
        (operation.x ? seed.x : seed.y) = value;
        prepared.set_registers(seed);
        require(prepared.run(1).retired == 1 &&
                    prepared.registers() == index_result(seed, operation),
                "Index lowering must wrap at 16 bits and change only Z");
        reference.set_registers(seed);
        static_cast<void>(reference.run(1));
        index_prefixes(prepared, reference, seed);
      }
    }
  }
}

struct ReentrantDevice {
  Core *core = nullptr;
  PreparationReport report;
  bool unchanged = false;

  static bool read(void *owner, Address address, Byte &value) noexcept {
    if (address != 0) { return false; }
    auto &device = *static_cast<ReentrantDevice *>(owner);
    const auto state = device.core->state();
    const auto progress = device.core->progress();
    try {
      device.report = device.core->prepare(entries);
    } catch (...) { return false; }
    device.unchanged = device.core->state() == state && device.core->progress() == progress;
    value = read_value;
    return true;
  }
};

void reentrant_preparation() {
  ReentrantDevice device;
  Io io{.owner = &device, .read = ReentrantDevice::read};
  Core core(Memory::external(), &io);
  device.core = &core;
  core.set_registers({.pc = entry});
  constexpr auto instruction =
      isa::program(isa::opcode<isa::Opcode::ldaa_direct>(), std::array<Byte, 1>{0});
  const auto result = core.run_instruction(instruction);
  require(
      result.retired == 1 && result.fault == Fault::none && device.unchanged &&
          device.report.failure.fault == Fault::reentrant && device.report.failure.retired == 0 &&
          core.registers().d == read_word,
      "Reentrant preparation must refuse without changing the active guest or its I/O transaction");
}

void independent_preparation() {
  constexpr Byte invalid = 0x18;
  constexpr Byte backedge = 0xfd;
  std::array<Byte, 5> bytes{invalid, invalid, static_cast<Byte>(isa::Opcode::dex), bra, backedge};
  const auto memory = Memory::window({.first = entry, .bytes = bytes, .writable = false});
  Core prepared(memory);
  Core reference(memory);
  constexpr std::array seeds{entry, Address{entry + 2}};
  const auto state = prepared.state();
  const auto progress = prepared.progress();
  const auto report = prepared.prepare(seeds);
  require(report.failure.fault == Fault::unsupported_instruction && report.failure.pc == entry &&
              report.failure.opcode == invalid && report.prepared_entries == 2 &&
              report.unresolved_entries == 0 && report.native_bytes != 0 &&
              prepared.state() == state && prepared.progress() == progress,
          "Independent valid entries must install without hiding invalid seed diagnostics or "
          "changing execution state");
  const Registers seed{
      .pc = entry + 2, .d = sentinel_x, .x = 1, .y = sentinel_y, .sp = stack, .ccr = 0xff};
  prepared.set_registers(seed);
  reference.set_registers(seed);
  constexpr std::uint32_t grant = 17;
  require(prepared.run(grant) == reference.run(grant) && prepared.state() == reference.state() &&
              prepared.progress() == reference.progress(),
          "A preparation diagnostic must not prevent exact independent native execution");
  const auto reused = prepared.prepare(seeds);
  require(reused.failure.fault == report.failure.fault && reused.failure.pc == report.failure.pc &&
              reused.prepared_entries == report.prepared_entries && reused.native_bytes == 0,
          "Repeated partial preparation must reuse valid native entries and retain diagnostics");
  prepared.set_registers({.pc = entry});
  reference.set_registers({.pc = entry});
  const auto invalid_run = prepared.run(1);
  require(invalid_run.fault == Fault::unsupported_instruction && invalid_run.retired == 0 &&
              invalid_run == reference.run(1) && prepared.state() == reference.state() &&
              prepared.progress() == reference.progress(),
          "Executing the invalid seed must still report the exact architectural failure");
}

void refusal_state() {
  auto bytes = loop;
  Core mutable_code(Memory::window({.first = entry, .bytes = bytes}));
  mutable_code.set_registers({.pc = entry, .d = sentinel_x});
  const auto state = mutable_code.state();
  const auto report = mutable_code.prepare(entries);
  require(report.prepared_entries == 0 && report.unresolved_entries == 1 &&
              report.native_bytes == 0 && report.failure.fault == Fault::none &&
              mutable_code.state() == state && mutable_code.progress() == Progress{},
          "Mutable instructions must remain unresolved without guest or I/O activity");
  constexpr Byte prefix = 0x18;
  std::array invalid{prefix, prefix};
  Core invalid_code(Memory::window({.first = entry, .bytes = invalid, .writable = false}));
  invalid_code.set_registers({.pc = entry, .d = sentinel_x});
  const auto before = invalid_code.state();
  const auto diagnostic = invalid_code.prepare(entries);
  require(diagnostic.failure.fault == Fault::unsupported_instruction &&
              diagnostic.failure.retired == 0 && diagnostic.failure.pc == entry &&
              diagnostic.failure.opcode == prefix && invalid_code.state() == before &&
              invalid_code.progress() == Progress{},
          "Preparation diagnostics must preserve complete state and never latch a guest failure");
}
}

int main() try {
  native_grants();
  retained_native_boundary();
  binding_lifetime();
  refusal_state();
  independent_preparation();
  arithmetic_entries();
  index_entries();
  reentrant_preparation();
  std::cout << "Prepared native installation, precise prefixes/deadlines and immutable binding "
               "lifetime passed\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
