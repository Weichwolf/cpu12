// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "tail_chain_model.hpp"

#include <array>
#include <span>
#include <stdexcept>

namespace cpu12::test::tail_chains {
namespace {
void cached_modes() {
  Image image{};
  image[vector] = 4;
  Core core(Memory::unified(image));
  core.set_registers(initial);
  require(!core.interrupt_frame_available() && core.interrupt({.vector = vector}).accepted &&
              core.run_instruction(rti).retired == 1 && core.registers() == initial,
          "Ordinary entry and real RTI establish a returned frame for a warm same-vector entry");
  const auto ordinary = image;
  accept(core, {.vector = vector});
  require(image == ordinary && core.run_instruction(rti).retired == 1 &&
              core.registers() == initial,
          "Returned-mode cache entry reuses the original same-vector frame without rewriting it");
  constexpr std::array<Byte, 1> wai{0x3e};
  require(core.run_instruction(wai).retired == 1 && core.state() == ExecutionState::waiting &&
              core.interrupt_frame_available() && !core.checkpoint().returned_frame,
          "WAI replaces returned-frame status with its genuine newly stacked waiting frame");
  const auto waiting = image;
  const auto saved_sp = core.registers().sp;
  require(core.interrupt({.vector = vector}).accepted && core.registers().sp == saved_sp &&
              image == waiting && core.run_instruction(rti).retired == 1,
          "Waiting-mode cache entry retains SP and memory for the same warm vector");
  require(core.run_instruction(nop).retired == 1 && !core.interrupt_frame_available() &&
              core.interrupt({.vector = vector}).accepted &&
              core.statistics().compiled_entries == 6,
          "All three frame modes retain distinct cached code, and ordinary entry works again after "
          "NOP");
}

void consecutive_returns() {
  Image image{};
  image[vector] = 4;
  put_frame(image, initial.sp);
  constexpr Address second = frame_sp + 9;
  put_frame(image, second);
  constexpr Byte high = 0x13;
  constexpr Byte low = 0x57;
  constexpr Address destination = 0x1357;
  image[second + 7U] = high;
  image[second + 8U] = low;
  Core core(Memory::unified(image));
  restore_frame(core, initial);
  require(core.run_instruction(rti).retired == 1 && core.registers().pc == destination &&
              core.registers().sp == second + 9U && core.interrupt_frame_available(),
          "Consecutive real RTIs replace the boundary with the latest restored frame");
  const auto frames = image;
  accept(core, {.vector = vector});
  require(core.registers().sp == second && image == frames,
          "Pending entry backs SP to the latest frame rather than a previously returned one");
}

void consumed_boundaries() {
  Image image{};
  image[vector] = 4;
  Core core(Memory::unified(image));
  constexpr std::array<std::array<Byte, 2>, 3> instructions{
      {{0xa7, 0}, {0x86, 0x7f}, {0x10, 0xef}}};
  constexpr Address loaded_d = 0x7f34;
  constexpr unsigned bits = 8;
  constexpr unsigned mask = 0xff;
  for (unsigned index = 0; index < instructions.size(); ++index) {
    put_frame(image, initial.sp);
    restore_frame(core, initial);
    const unsigned length = index == 0 ? 1 : 2;
    auto wanted = restored(initial);
    wanted.pc = static_cast<Address>(wanted.pc + length);
    if (index == 1) { wanted.d = loaded_d; }
    require(core.run_instruction(std::span(instructions[index]).first(length)).retired == 1 &&
                core.registers() == wanted && !core.interrupt_frame_available() &&
                !core.checkpoint().returned_frame,
            "Every actual following NOP/load/CCR instruction consumes the old return boundary");
    require(
        core.interrupt({.vector = vector}).accepted && image[frame_sp] == wanted.ccr &&
            image[frame_sp + 2U] == wanted.d >> bits && image[frame_sp + 8U] == (wanted.pc & mask),
        "Later normal entry saves current registers and advanced PC through real new stack writes");
  }
}

void restored_boundary() {
  Image image{};
  image[vector] = 4;
  put_frame(image, initial.sp);
  Core core(Memory::unified(image));
  restore_frame(core, initial);
  const auto cpu = core.checkpoint();
  const auto frame = image;
  core.clear_code_cache();
  require(core.interrupt_frame_available() && core.checkpoint() == cpu,
          "Code invalidation leaves the genuine completed return boundary intact");
  require(core.run_instruction(nop).retired == 1 && !core.interrupt_frame_available(),
          "Fixture consumes the return boundary with one real instruction");
  const auto retired = core.statistics().native_instructions;
  core.restore(cpu);
  require(
      core.checkpoint() == cpu && core.interrupt_frame_available() &&
          core.statistics().native_instructions == retired,
      "A CPU checkpoint restores return eligibility without rewinding native execution statistics");
  accept(core, {.vector = vector});
  require(image == frame && core.statistics().native_instructions == retired,
          "Restored boundary service consumes only the actual vector and leaves the saved frame "
          "intact");
}

void fresh_boundaries() {
  Image image{};
  image[vector] = 4;
  put_frame(image, initial.sp);
  Core core(Memory::unified(image));
  restore_frame(core, initial);
  const auto cpu = core.checkpoint();
  std::array<Checkpoint, 2> invalid{cpu, cpu};
  invalid[0].irq_delay = 1;
  invalid[1].state = ExecutionState::waiting;
  for (const auto &candidate : invalid) {
    bool refused = false;
    try {
      core.restore(candidate);
    } catch (const std::invalid_argument &) { refused = true; }
    require(refused && core.checkpoint() == cpu,
            "A returned frame cannot coexist with IRQ delay or a waiting checkpoint state");
  }
  core.set_registers(core.registers());
  require(!core.interrupt_frame_available() && !core.checkpoint().returned_frame,
          "Explicit register replacement is a fresh boundary even when the values are identical");
  restore_frame(core, initial);
  require(core.reset(vector).accepted && !core.interrupt_frame_available() &&
              !core.checkpoint().returned_frame && core.checkpoint().irq_delay == 0,
          "Actual CPU reset clears returned-frame and deferral status");
}

void irq_deferral() {
  Image image{};
  image[vector] = 4;
  constexpr Byte masked = 0x30;
  put_frame(image, initial.sp, masked);
  Core core(Memory::unified(image));
  restore_frame(core, initial, masked);
  constexpr std::array<Byte, 2> cli{0x10, 0xef};
  require(core.run_instruction(cli).retired == 1 && !core.interrupt_frame_available() &&
              !core.checkpoint().returned_frame && core.checkpoint().irq_delay == 1 &&
              !core.interrupt_ready(InterruptKind::maskable),
          "CLI replaces returned-frame status with the genuine following-instruction IRQ delay");
  core.clear_code_cache();
  require(core.run_instruction(nop).retired == 1 && core.interrupt_ready(InterruptKind::maskable) &&
              !core.interrupt_frame_available() && core.interrupt({.vector = vector}).accepted,
          "After the following instruction, normal entry stacks current context instead of old "
          "return bytes");
  constexpr Byte unmasked = 0x20;
  require(image[frame_sp] == unmasked,
          "Current post-CLI flags replace the earlier saved I bit on actual ordinary entry");
}

void clocked_boundary() {
  Image image{};
  image[vector] = 4;
  put_frame(image, initial.sp);
  Core core(Memory::unified(image));
  restore_frame(core, initial);
  auto expected = core.checkpoint();
  expected.returned_frame = false;
  const auto storage = image;
  const auto retired = core.statistics().native_instructions;
  core.close_return_window();
  require(core.checkpoint() == expected && !core.interrupt_frame_available() && image == storage &&
              core.statistics().native_instructions == retired,
          "Completing the board's RTI poll closes only return eligibility without memory or "
          "instruction effects");
  constexpr Byte restored_ccr = 0x20;
  require(core.interrupt({.vector = vector}).accepted && image[frame_sp] == restored_ccr,
          "An interrupt arriving during later refill clocks creates a genuine new frame");
  auto before = initial;
  constexpr Byte masked = 0x10;
  before.ccr = masked;
  core.set_registers(before);
  constexpr std::array<Byte, 2> cli{0x10, 0xef};
  require(core.run_instruction(cli).retired == 1, "Fixture establishes real CLI deferral");
  const auto deferred = core.checkpoint();
  core.close_return_window();
  require(core.checkpoint() == deferred && deferred.irq_delay == 1 &&
              !core.interrupt_ready(InterruptKind::maskable),
          "Closing a return window cannot consume an unrelated IRQ delay");
  constexpr std::array<Byte, 1> wai{0x3e};
  require(core.run_instruction(wai).retired == 1, "Fixture enters the actual native waiting state");
  const auto waiting = core.checkpoint();
  core.close_return_window();
  require(core.checkpoint() == waiting && core.interrupt_frame_available(),
          "The board's return poll cannot discard a genuine WAI frame");
}
}

void checkpoint_cases() {
  cached_modes();
  consecutive_returns();
  consumed_boundaries();
  restored_boundary();
  fresh_boundaries();
  irq_deferral();
  clocked_boundary();
}
}
