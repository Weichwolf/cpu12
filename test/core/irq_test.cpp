// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "irq_model.hpp"

#include <array>
#include <exception>
#include <iostream>

namespace {
using namespace cpu12;
using namespace cpu12::test::irqs;

void masks(Core &core, Image &image) {
  constexpr unsigned maximum = 0xff;
  for (const auto kind : {InterruptKind::maskable, InterruptKind::xirq}) {
    const InterruptRequest request{
        .vector = static_cast<Address>(vector + (kind == InterruptKind::xirq ? 2U : 0U)),
        .kind = kind};
    for (unsigned ccr = 0; ccr <= maximum; ++ccr) {
      auto before = seed;
      before.ccr = static_cast<Byte>(ccr);
      core.set_registers(before);
      prepare_vector(image, request.vector);
      constexpr unsigned irq_mask = 0x10;
      constexpr unsigned xirq_mask = 0x40;
      const auto blocked = (ccr & (kind == InterruptKind::xirq ? xirq_mask : irq_mask)) != 0;
      require(core.interrupt_ready(kind) != blocked,
              "IRQ and XIRQ readiness follows their distinct masks for every CCR value");
      if (blocked) {
        const auto result = core.interrupt(request);
        require(!result.accepted && result.fault == Fault::none && core.registers() == before,
                "A masked request causes no fabricated event or CPU state change");
      } else {
        accept(core, before, request, image);
      }
    }
  }
}

void stack_positions(Core &core, Image &image) {
  constexpr unsigned maximum = 0xffff;
  for (unsigned sp = 0; sp <= maximum; ++sp) {
    auto before = seed;
    before.sp = static_cast<Address>(sp);
    const auto lower = static_cast<Address>(sp + address_bytes - 10U);
    image[lower] = 1;
    image[before.sp] = 2;
    prepare_vector(image, vector);
    const auto guard_low = image[lower];
    const auto guard_high = image[before.sp];
    core.set_registers(before);
    accept(core, before, {.vector = vector}, image);
    require(image[lower] == guard_low && image[before.sp] == guard_high,
            "Nine-byte IRQ pushes preserve guards at every wrapped stack position");
  }
}

void return_path() {
  Image image{};
  prepare_vector(image, vector);
  constexpr Byte rti = 0x0b;
  constexpr Byte nop = 0xa7;
  image[handler] = rti;
  image[seed.pc] = nop;
  Core core(Memory::unified(image));
  core.set_registers(seed);
  accept(core, seed, {.vector = vector}, image);
  auto wanted = seed;
  ++wanted.pc;
  const auto result = core.run(2);
  require(result.retired == 2 && result.fault == Fault::none && core.registers() == wanted &&
              core.statistics().native_instructions == 2 && core.statistics().compiled_entries == 3,
          "Native IRQ entry and fetched RTI return to the genuine interrupted next instruction");
}

void deferral() {
  Image image{};
  prepare_vector(image, vector);
  Core core(Memory::unified(image));
  auto before = seed;
  constexpr Byte old_mask = 0x10;
  before.ccr = old_mask;
  constexpr std::array<Byte, 2> cli{0x10, 0xef};
  constexpr std::array<Byte, 1> nop{0xa7};
  core.set_registers(before);
  require(core.run_instruction(cli).retired == 1 &&
              !core.interrupt_ready(InterruptKind::maskable) &&
              core.interrupt_ready(InterruptKind::xirq),
          "CLI clearing I defers maskable IRQ while leaving XIRQ eligible");
  const auto delayed = core.interrupt({.vector = vector});
  require(!delayed.accepted && delayed.fault == Fault::none,
          "A deferred request performs no interrupt entry");
  require(
      !core.interrupt_ready(InterruptKind::maskable) && core.run_instruction(nop).retired == 1 &&
          core.interrupt_ready(InterruptKind::maskable),
      "Deferral survives cache invalidation and expires after one following retired instruction");
  accept(core, core.registers(), {.vector = vector}, image);
  for (const Byte selector : {Byte{2}, Byte{10}}) {
    before.d = 0;
    core.set_registers(before);
    const std::array<Byte, 2> tap{0xb7, selector};
    require(
        core.run_instruction(tap).retired == 1 && !core.interrupt_ready(InterruptKind::maskable) &&
            core.run_instruction(nop).retired == 1 && core.interrupt_ready(InterruptKind::maskable),
        "TAP and its ignored-bit alias preserve the following-instruction guarantee");
  }
  core.set_registers(before);
  require(core.run_instruction(cli).retired == 1 && !core.interrupt_ready(InterruptKind::maskable),
          "Fixture reestablishes the pending maskable-IRQ delay");
  accept(core, core.registers(), {.vector = vector, .kind = InterruptKind::xirq}, image);
  constexpr std::array<Byte, 1> rti{0x0b};
  require(core.run_instruction(rti).retired == 1 && core.interrupt_ready(InterruptKind::maskable),
          "XIRQ can preempt the delay and RTI restores the actual prior unmasked state");
  before.ccr = 0;
  core.set_registers(before);
  require(core.run_instruction(cli).retired == 1 && core.interrupt_ready(InterruptKind::maskable),
          "An already clear I bit does not rearm the CLI transition delay");
}
}

int main() {
  try {
    Image image{};
    Core core(Memory::unified(image));
    masks(core, image);
    stack_positions(core, image);
    require(core.statistics().native_instructions == 0 && core.statistics().read_callbacks == 0 &&
                core.statistics().write_callbacks == 0,
            "Full-RAM interrupt entry uses native memory and retires no firmware instruction");
    return_path();
    deferral();
    bus_cases();
    std::cout << "Verified " << entered
              << " interrupt entries, all CCR/stack values, native return paths and precise bus "
                 "faults.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
