// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "wait_model.hpp"

#include <array>
#include <stdexcept>

namespace cpu12::test::waits {
namespace {
void restored_waits() {
  Image image{};
  image[vector] = 4;
  for (const auto form : forms) {
    Core core(Memory::unified(image));
    core.set_registers(seed);
    enter(core, image, seed, form);
    const auto cpu = core.checkpoint();
    const auto storage = image;
    require(core.interrupt({.vector = vector}).accepted && core.run_instruction(rti).retired == 1,
            "Checkpoint fixture first completes a real native wake and return");
    auto replacement = seed;
    replacement.pc = handler;
    core.set_registers(replacement);
    core.clear_code_cache();
    const auto retired = core.statistics().native_instructions;
    image = storage;
    core.restore(cpu);
    require(
        core.checkpoint() == cpu && core.run(idle_budget).retired == 0 &&
            core.run(idle_budget).state == form.state &&
            core.statistics().native_instructions == retired,
        "CPU and caller-owned storage restore a real idle boundary without rewinding statistics");
    require(core.interrupt({.vector = vector}).accepted && core.run_instruction(rti).retired == 1 &&
                core.registers().pc == sleeping(seed, form).pc && core.registers().sp == seed.sp &&
                core.registers().d == seed.d && core.registers().ccr == seed.ccr,
            "A restored wait reuses the saved real frame and returns to the actual following PC");
  }
}

void deferred_irq() {
  Image image{};
  image[vector] = 4;
  Core core(Memory::unified(image));
  auto before = seed;
  constexpr Byte masked = 0x10;
  before.ccr = masked;
  core.set_registers(before);
  constexpr std::array<Byte, 2> cli{0x10, 0xef};
  require(core.run_instruction(cli).retired == 1 && !core.interrupt_ready(InterruptKind::maskable),
          "CLI establishes the architectural following-instruction delay");
  const auto saved = core.checkpoint();
  require(saved.irq_delay == 1 && core.run_instruction(nop).retired == 1 &&
              core.interrupt_ready(InterruptKind::maskable),
          "Checkpoint retains the one outstanding instruction boundary");
  core.restore(saved);
  core.clear_code_cache();
  require(core.checkpoint() == saved && !core.interrupt_ready(InterruptKind::maskable) &&
              !core.interrupt({.vector = vector}).accepted,
          "Restoring and clearing code cannot silently consume IRQ deferral");
  enter(core, image, saved.registers, forms[0]);
  require(core.interrupt_ready(InterruptKind::maskable) &&
              core.interrupt({.vector = vector}).accepted,
          "WAI is the genuine following instruction and then permits an eligible wake");
}

void invalid_checkpoints() {
  Image image{};
  Core core(Memory::unified(image));
  core.set_registers(seed);
  enter(core, image, seed, forms[0]);
  const auto original = core.checkpoint();
  std::array<Checkpoint, 4> invalid{original, original, original, original};
  invalid[0].irq_delay = 2;
  invalid[1].irq_delay = 1;
  invalid[2].state = ExecutionState::stopped;
  constexpr Byte disabled = 0x80;
  invalid[2].registers.ccr = disabled;
  invalid[3].failure.fault = Fault::memory_read;
  invalid[3].failure.pc = handler;
  for (const auto &candidate : invalid) {
    bool rejected = false;
    try {
      core.restore(candidate);
    } catch (const std::invalid_argument &) { rejected = true; }
    require(
        rejected && core.checkpoint() == original,
        "Invalid deferral, disabled STOP or mismatched fault PC is rejected before CPU mutation");
  }
}

void reset_boundaries() {
  Image image{};
  image[vector] = 4;
  Core core(Memory::unified(image));
  for (const auto form : forms) {
    core.set_registers(seed);
    enter(core, image, seed, form);
    const auto idle = core.registers();
    require(core.reset(vector).accepted && core.state() == ExecutionState::running &&
                core.registers().pc == handler && core.registers().sp == idle.sp &&
                core.checkpoint().irq_delay == 0,
            "Accepted reset clears wait state while retaining the explicit reset SP policy");
    require(!core.resume_from_stop().accepted,
            "A masked-XIRQ continuation cannot be applied to a running CPU");
    core.set_registers(seed);
    enter(core, image, seed, form);
    core.set_registers(seed);
    require(core.state() == ExecutionState::running && core.checkpoint().irq_delay == 0 &&
                core.registers() == seed,
            "An explicit debug register boundary cancels idle and deferral state");
  }
}

void diagnostic_snapshot() {
  unsigned reads = 0;
  constexpr Byte diagnostic = 0xa5;
  Io io{.owner = &reads,
        .read_masked = [](void *owner, MaskedByteRead /*access*/, Byte &value) noexcept {
          ++*static_cast<unsigned *>(owner);
          value = diagnostic;
          return false;
        }};
  Core core(Memory::external(), &io);
  core.set_registers(seed);
  constexpr std::array<Byte, 5> branch{0x1e, 0xab, 0xcd, 0x0f, 0};
  const auto result = core.run_instruction(branch);
  constexpr RunResult expected{.fault = Fault::memory_read,
                               .pc = seed.pc,
                               .opcode = 0x1e,
                               .address = 0xabcd,
                               .value = diagnostic,
                               .access_size = 1,
                               .read_mask = 0x0f};
  const auto saved = core.checkpoint();
  require(result == expected && saved.failure == expected && reads == 1,
          "Checkpoint stores independently expected PC/opcode/address/width/mask and actual "
          "diagnostic byte");
  core.set_registers(seed);
  core.restore(saved);
  require(
      core.step() == expected && reads == 1 && core.registers() == seed,
      "Restoring a selected-read fault preserves every diagnostic field without rereading the bus");
}
}

void checkpoint_cases() {
  restored_waits();
  deferred_irq();
  invalid_checkpoints();
  reset_boundaries();
  diagnostic_snapshot();
}
}
