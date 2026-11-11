// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_transitions.hpp"
#include "interrupt_model.hpp"
#include "isa.hpp"

#include <array>
#include <span>

namespace cpu12::test::transitions {
namespace {
void indirect(const NativeImage &image, const Address target, const bool declared) {
  constexpr auto source =
      isa::program(isa::opcode<isa::Opcode::jmp_indexed>(), std::array<Byte, 1>{0});
  Fixture fixture(source, payload, true);
  Core core(fixture.memory());
  core.install(image, from);
  auto initial = seed;
  initial.x = target;
  core.set_registers(initial);
  const auto result = core.run(8);
  initial.pc = target;
  require(result.retired == 1 && core.registers() == initial &&
              core.progress().estimated_cycles == 3 &&
              result.fault == (declared ? Fault::none : Fault::unprepared_execution),
          "A real indexed jump yields only to its declared target and otherwise preserves its "
          "completed fault prefix");
  if (!declared) { return; }
  core.set_memory(fixture.memory(true));
  core.install(image, to);
  require(core.step().retired == 1, "An indexed target resumes the already prepared version");
}

void call_return() {
  constexpr Address continuation = start + 3;
  constexpr auto source = isa::program(jump(destination, true), loop);
  constexpr std::array entries{start, continuation};
  constexpr std::array transfers{forward[0],
                                 CodeTransition{.from = to, .to = from, .entry = continuation}};
  const auto image = compile(source, isa::rts, transfers, entries);
  Fixture fixture(source, isa::rts, true);
  Core core(fixture.memory());
  core.install(image, from);
  core.set_registers(seed);
  auto expected = seed;
  expected.pc = destination;
  expected.sp -= 2;
  auto bytes = fixture.bytes;
  bytes[expected.sp] = static_cast<Byte>(continuation >> byte_bits);
  bytes[expected.sp + 1] = static_cast<Byte>(continuation);
  require(core.run(32).retired == 1 &&
              core.state() == State{.registers = expected, .failure = {}} &&
              core.progress().estimated_cycles == 4 && fixture.bytes == bytes,
          "Cross-version JSR pushes its real return address once before yielding");
  core.set_memory(fixture.memory(true));
  core.install(image, to);
  expected.pc = continuation;
  expected.sp = seed.sp;
  require(
      core.run(32).retired == 1 && core.state() == State{.registers = expected, .failure = {}} &&
          core.progress().estimated_cycles == 9 && fixture.bytes == bytes,
      "The actual prepared RTS consumes its stack and requests the original continuation variant");
  core.set_memory(fixture.memory());
  core.install(image, from);
  require(core.run(8).retired == 8 && core.registers() == expected && fixture.bytes == bytes &&
              core.progress().estimated_cycles == 9 + (8 * 3) &&
              core.statistics().compiled_entries == 0,
          "The prepared caller continues without replaying JSR, RTS or their version transitions");
}

void irq(const bool returned, const bool reuse) {
  const auto source = returned ? std::span<const Byte>(isa::rti) : std::span<const Byte>(loop);
  const auto image = compile(source);
  Fixture fixture(source, payload, true);
  Core core(fixture.memory());
  core.install(image, from);
  core.set_registers(seed);
  auto expected = seed;
  auto bytes = fixture.bytes;
  if (returned) {
    expected.pc = destination;
    test::interrupts::put(fixture.bytes, seed.sp, test::interrupts::encode(expected));
    bytes = fixture.bytes;
    expected.sp += 9;
    require(core.run(8).retired == 1 && core.state().returned_frame &&
                core.progress().estimated_cycles == 8,
            "A real RTI can reach a declared prepared variant while retaining its return window");
  } else {
    expected.pc = destination;
    expected.sp -= 9;
    expected.ccr |= 0x10;
    test::interrupts::put(bytes, expected.sp, test::interrupts::encode(seed));
    require(core.interrupt(requests[0]).accepted && core.progress().estimated_cycles == 9,
            "A real vector/frame IRQ can transfer directly into a different prepared variant");
  }
  const auto state = core.state();
  const auto progress = core.progress();
  require(core.registers() == expected && fixture.bytes == bytes && core.run(8).retired == 0,
          "Interrupt/return transfer waits without replaying vector, frame or data effects");
  core.set_memory(fixture.memory(true));
  core.install(image, to);
  require(core.state() == state && core.progress() == progress && fixture.bytes == bytes,
          "Version binding preserves hidden IRQ/return state as well as all public registers");
  if (reuse) {
    expected.sp -= 9;
    expected.ccr |= 0x10;
    require(
        core.interrupt(requests[0]).accepted && core.registers() == expected &&
            fixture.bytes == bytes &&
            core.progress().estimated_cycles == progress.estimated_cycles + 6,
        "An IRQ after a cross-version RTI reuses the original frame instead of pushing a new one");
  } else {
    require(
        core.step().retired == 1 && !core.state().returned_frame &&
            core.progress().estimated_cycles == progress.estimated_cycles + 1,
        "The first prepared target instruction closes a preserved returned-frame window normally");
  }
}
}

void transfers() {
  constexpr auto source =
      isa::program(isa::opcode<isa::Opcode::jmp_indexed>(), std::array<Byte, 1>{0});
  const auto image = compile(source);
  indirect(image, destination, true);
  indirect(image, destination + 1, false);
  call_return();
  irq(false, false);
  irq(true, false);
  irq(true, true);
}
}
