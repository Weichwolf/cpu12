// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "observed_core.hpp"
#include "wait_model.hpp"

#include <array>

namespace cpu12::test::waits {
namespace {

void deferred_irq() {
  Image image{};
  image[vector] = 4;
  ObservedCore core(Memory::unified(image));
  auto before = seed;
  constexpr Byte masked = 0x10;
  before.ccr = masked;
  core.set_registers(before);
  constexpr std::array<Byte, 2> cli{0x10, 0xef};
  require(core.run_instruction(cli).retired == 1 && !core.interrupt_ready(InterruptKind::maskable),
          "CLI establishes the architectural following-instruction delay");
  const auto saved = core.state();
  require(saved.irq_delay == 1 && core.state() == saved &&
              !core.interrupt({.vector = vector}).accepted,
          "Read-only inspection and code inspection preserves the pending IRQ delay");
  enter(core, image, saved.registers, forms[0]);
  require(core.interrupt_ready(InterruptKind::maskable) &&
              core.interrupt({.vector = vector}).accepted,
          "WAI is the genuine following instruction and then permits an eligible wake");
}

void reset_boundaries() {
  Image image{};
  image[vector] = 4;
  ObservedCore core(Memory::unified(image));
  for (const auto form : forms) {
    core.set_registers(seed);
    enter(core, image, seed, form);
    const auto idle = core.registers();
    require(core.reset(vector).accepted && core.state().execution == ExecutionState::running &&
                core.registers().pc == handler && core.registers().sp == idle.sp &&
                core.state().irq_delay == 0,
            "Accepted reset clears wait state while retaining the explicit reset SP policy");
    require(!core.resume_from_stop().accepted,
            "A masked-XIRQ continuation cannot be applied to a running CPU");
    core.set_registers(seed);
    enter(core, image, seed, form);
    core.set_registers(seed);
    require(core.state().execution == ExecutionState::running && core.state().irq_delay == 0 &&
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
  ObservedCore core(Memory::external(), &io);
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
  const auto saved = core.state();
  require(result == expected && saved.failure == expected && reads == 1,
          "State inspection reports independently expected PC/opcode/address/width/mask and actual "
          "diagnostic byte");
  require(
      core.step() == expected && reads == 1 && core.registers() == seed,
      "A latched selected-read fault preserves every diagnostic field without rereading the bus");
}
}

void state_cases() {
  deferred_irq();
  reset_boundaries();
  diagnostic_snapshot();
}
}
