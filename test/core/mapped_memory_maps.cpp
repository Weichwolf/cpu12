// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "mapped_memory.hpp"
#include "observed_core.hpp"

#include <array>
#include <span>
#include <stdexcept>

namespace cpu12::test::mapped {
namespace {
constexpr Byte first_value = 0x11, second_value = 0x22, original_a = 0x12;
constexpr Address first_result = 0x1134, second_result = 0x2234;

void remapping() {
  Storage bytes;
  auto a = bytes.grants();
  auto b = a;
  b[0].bytes = bytes.high;
  b[0].known = bytes.high_known;
  b[1].bytes = bytes.low;
  b[1].known = bytes.low_known;
  bytes.low[0] = first_value;
  bytes.low_known[0] = 1;
  bytes.high[0] = second_value;
  bytes.high_known[0] = 1;
  ObservedCore core(Memory::mapped(a));
  constexpr auto load = extended<isa::Opcode::ldaa_extended>(first);
  for (unsigned pass = 0; pass < 4; ++pass) {
    core.set_registers(seed);
    const auto before = core.state();
    core.set_memory(Memory::mapped((pass % 2) == 0 ? a : b));
    require(core.state() == before, "Explicit remapping changes no register, IRQ or failure state");
    require(core.run_instruction(load).retired == 1 &&
                core.registers().d == ((pass % 2) == 0 ? first_result : second_result) &&
                core.statistics().compiled_entries == (pass == 0 ? 1U : 2U),
            "Returning to a physical mapping reuses its immutable native translation");
  }
  const auto count = core.statistics().compiled_entries;
  b[0].identity = 1;
  core.set_memory(Memory::mapped(b));
  core.set_registers(seed);
  require(core.run_instruction(load).retired == 1 &&
              core.statistics().compiled_entries == count + 1U,
          "An explicit changed region identity selects a distinct retained translation variant");
  a[0].bytes = {};
  core.set_registers(seed);
  require(core.run_instruction(load).retired == 1 && core.registers().d == second_result,
          "Caller descriptor mutation cannot silently remap the ObservedCore's copied grants");
}

void alias_and_permissions() {
  Storage bytes;
  auto grants = bytes.grants();
  grants[1].bytes = bytes.low;
  grants[1].known = bytes.low_known;
  Owner owner;
  auto io = owner.io();
  ObservedCore core(Memory::mapped(grants), &io);
  constexpr auto store = extended<isa::Opcode::staa_extended>(first + 2);
  constexpr auto load = extended<isa::Opcode::ldaa_extended>(second + 2);
  core.set_registers(seed);
  require(core.run_instruction(store).retired == 1, "Mapped alias store succeeds");
  core.set_registers(seed);
  require(core.run_instruction(load).retired == 1 && core.registers().d == seed.d &&
              owner.reads == 0,
          "Physical aliases share real data and live knownness without copies");
  grants[0].writable = false;
  core.set_memory(Memory::mapped(grants));
  core.set_registers(seed);
  owner.core = &core;
  const auto failed = core.run_instruction(store);
  require(failed.fault == Fault::memory_write && owner.writes == 1 && owner.refused_remap &&
              bytes.low[2] == original_a && core.registers() == seed,
          "Read-only writes use the actual owner and active remapping is refused");
  core.set_registers(seed);
  constexpr auto increment = extended<isa::Opcode::inc_extended>(first + 2);
  const auto modified = core.run_instruction(increment);
  require(modified.fault == Fault::memory_write && owner.reads == 0 && owner.writes == 2 &&
              core.registers() == seed && bytes.low[2] == original_a,
          "Read-only RMW reads known data natively and reports refusal of the actual write");
}

void fetch_and_boundaries() {
  Storage bytes;
  auto grants = bytes.grants();
  bytes.high[0] = isa::nop[0];
  bytes.high_known[0] = 1;
  Owner owner;
  auto io = owner.io();
  ObservedCore core(Memory::mapped(grants), &io);
  core.set_registers({.pc = second});
  require(core.step().retired == 1 && owner.reads == 0,
          "Normal architectural fetch uses a known mapped program byte directly");
  bytes.high_known[0] = 0;
  core.set_registers({.pc = second});
  require(core.step().fault == Fault::instruction_fetch && owner.address == second &&
              owner.reads == 1,
          "Architectural fetch observes newly unknown mapped bytes through its actual owner");
  grants[1].first = first + bytes.low.size();
  core.set_memory(Memory::mapped(grants));
  bytes.low_known.back() = 1;
  bytes.high_known[0] = 1;
  core.set_registers(seed);
  constexpr auto cross = extended<isa::Opcode::ldd_extended>(first + 15);
  const auto failed = core.run_instruction(cross);
  require(failed.fault == Fault::memory_read && failed.address == first + 15 && owner.width == 2 &&
              core.registers() == seed,
          "A cross-grant word preserves one owner word operation and precise refusal");
}

void boundary_state() {
  std::array<Byte, 64> bytes{};
  ObservedCore core(Memory::unified(bytes));
  auto initial = seed;
  initial.sp = 0x20;
  initial.ccr = 0x10;
  core.set_registers(initial);
  constexpr auto cli =
      isa::program(isa::opcode<isa::Opcode::andcc_immediate>(), std::array<Byte, 1>{0xef});
  require(core.run_instruction(cli).retired == 1 && core.state().irq_delay == 1,
          "The mapped-memory boundary test establishes a real deferred IRQ");
  const auto deferred = core.state();
  core.set_memory(Memory::external());
  require(core.state() == deferred && !core.interrupt_ready(InterruptKind::maskable),
          "Changing memory bindings preserves an actual pending IRQ delay");
  core.set_memory(Memory::unified(bytes));
  require(core.run_instruction(isa::wai).retired == 1,
          "The pending CPU continues into its actual native WAI after rebinding");
  const auto waiting = core.state();
  core.set_memory(Memory::external());
  require(core.state() == waiting && core.interrupt_frame_available(),
          "A memory identity change preserves the complete real WAI boundary");
  core.set_memory(Memory::unified(bytes));
  core.set_registers(waiting.registers);
  require(core.run_instruction(isa::rti).retired == 1 && core.state().returned_frame,
          "The independent saved frame establishes an actual RTI return window");
  const auto returned = core.state();
  core.set_memory(Memory::external());
  require(core.state() == returned && core.interrupt_frame_available(),
          "An RTI frame remains available across an explicit memory transition");
  constexpr auto missing = extended<isa::Opcode::ldaa_extended>(first);
  const auto failed = core.run_instruction(missing);
  const auto faulted = core.state();
  core.set_memory(Memory::unified(bytes));
  require(failed.fault == Fault::memory_read && core.state() == faulted && core.step() == failed,
          "Remapping never clears or replays an existing architectural fault");
}

void validation() {
  Storage bytes;
  auto grants = bytes.grants();
  grants[1].first = first;
  bool overlap = false;
  try {
    static_cast<void>(Memory::mapped(grants));
  } catch (const std::invalid_argument &) { overlap = true; }
  require(overlap, "Ambiguous logical grants are rejected before native compilation");
  grants = bytes.grants();
  grants[0].known = std::span(bytes.low_known).first(1);
  bool known = false;
  try {
    static_cast<void>(Memory::mapped(grants));
  } catch (const std::invalid_argument &) { known = true; }
  require(known, "Knownness bounds must exactly match the byte backing");
}
}

void mapping_cases() {
  remapping();
  alias_and_permissions();
  fetch_and_boundaries();
  boundary_state();
  validation();
}
}
