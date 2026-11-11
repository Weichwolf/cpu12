// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_support.hpp"
#include "interrupt_model.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cpu12::test::image {
namespace {
namespace frames = cpu12::test::interrupts;
constexpr Registers seed{
    .pc = entry, .d = 0x1234, .x = 0x5678, .y = 0x9abc, .sp = 0x9000, .ccr = 0x20};
constexpr Address handler = cases[0] + 2;
constexpr unsigned xirq_masks = 0x50;
enum class Frame : std::uint8_t { push, waiting, returned };

void put_vector(Fixture &fixture, const Address target) {
  fixture.bytes[vector] = static_cast<Byte>(target >> byte_bits);
  fixture.bytes[vector + 1] = static_cast<Byte>(target);
}

void seed_frame(Core &core, Fixture &fixture, const Frame frame) {
  core.set_registers(seed);
  if (frame == Frame::waiting) {
    require(core.run_instruction(isa::wai).retired == 1 &&
                core.state().execution == ExecutionState::waiting,
            "The cold handover fixture enters a genuine native WAI state");
  } else if (frame == Frame::returned) {
    frames::put(fixture.bytes, seed.sp, frames::encode(seed));
    require(core.run_instruction(isa::rti).retired == 1 && core.state().returned_frame,
            "The cold handover fixture opens a genuine native RTI return window");
  }
}

void event(const NativeImage &image, const Frame frame, const InterruptRequest request) {
  Fixture fixture{Source(0x80)};
  put_vector(fixture, handler);
  Core core(fixture.memory());
  seed_frame(core, fixture, frame);
  const auto before = core.state();
  const auto progress = core.progress();
  auto memory = fixture.bytes;
  core.install(image, first_id);
  const auto compiled = core.statistics().compiled_entries;
  require(core.state() == before && core.progress() == progress && fixture.bytes == memory,
          "Cold IRQ preparation cannot replay or alter an existing architectural frame");
  auto expected = before.registers;
  expected.pc = handler;
  if (frame != Frame::waiting) { expected.sp -= 9; }
  expected.ccr =
      static_cast<Byte>(expected.ccr | (request.kind == InterruptKind::xirq ? xirq_masks : 0x10U));
  if (frame == Frame::push) { frames::put(memory, expected.sp, frames::encode(before.registers)); }
  const auto result = core.interrupt(request);
  const auto cycles = progress.estimated_cycles + (frame == Frame::push ? 9U : 6U);
  require(
      result.accepted && result.fault == Fault::none && result.pc == before.registers.pc &&
          core.state() == State{.registers = expected, .failure = {}} &&
          core.progress() == Progress{.estimated_cycles = cycles} && fixture.bytes == memory &&
          core.statistics().compiled_entries == compiled,
      "Prepared IRQ contracts preserve exact push/wait/return frames, masks, memory and cycles");
  require(core.run(1).retired == 1 && core.registers() == expected &&
              core.progress().estimated_cycles == cycles + 3 &&
              core.statistics().compiled_entries == compiled,
          "The real vector enters prepared firmware without compiling the first IRQ or handler");
}

void undeclared(const NativeImage &image) {
  Fixture fixture{Source(0x80)};
  Core core(fixture.memory());
  core.install(image, first_id);
  auto masked = seed;
  masked.ccr |= 0x10;
  core.set_registers(masked);
  constexpr InterruptRequest unknown{.vector = vector + 2};
  const auto memory = fixture.bytes;
  const auto progress = core.progress();
  const auto compiled = core.statistics().compiled_entries;
  const auto blocked = core.interrupt(unknown);
  require(!blocked.accepted && blocked.fault == Fault::none && core.registers() == masked,
          "A masked undeclared event still performs no hardware transaction");
  core.set_registers(seed);
  const auto missing = core.interrupt(unknown);
  require(!missing.accepted && missing.fault == Fault::unprepared_execution &&
              missing.address == unknown.vector && missing.pc == seed.pc &&
              core.registers() == seed && core.progress() == progress && fixture.bytes == memory &&
              core.statistics().compiled_entries == compiled,
          "An undeclared IRQ contract fails before frame/vector I/O or first-use compilation");
}

void wake(const NativeImage &image, const bool remap) {
  Fixture fixture{Source(0x80)};
  Core core(fixture.memory());
  auto before = seed;
  before.ccr |= 0x40;
  core.set_registers(before);
  require(core.run_instruction(isa::stop).state == ExecutionState::stopped,
          "The wake fixture enters the actual architectural STOP state");
  core.install(image, first_id);
  if (remap) { core.set_memory(fixture.memory()); }
  const auto stopped = core.state();
  const auto progress = core.progress();
  const auto memory = fixture.bytes;
  const auto compiled = core.statistics().compiled_entries;
  const auto result = core.resume_from_stop();
  if (remap) {
    require(!result.accepted && result.fault == Fault::unprepared_execution &&
                core.registers() == stopped.registers &&
                core.state().execution == ExecutionState::stopped,
            "An invalidated STOP-wake binding cannot lazily emit native code or report acceptance");
  } else {
    before.pc += 2;
    require(result.accepted && result.fault == Fault::none &&
                core.state() == State{.registers = before, .failure = {}},
            "Prepared masked-XIRQ STOP wake restores only the real continuation and SP/state");
  }
  require(core.progress() == progress && fixture.bytes == memory &&
              core.statistics().compiled_entries == compiled,
          "STOP-wake events perform no hidden vector/frame reads, writes or translation");
}

void returned_instruction(const NativeImage &image) {
  Fixture fixture{Source(0x80)};
  Core core(fixture.memory());
  auto before = seed;
  before.pc = handler;
  frames::put(fixture.bytes, before.sp, frames::encode(before));
  core.set_registers(before);
  require(core.run_instruction(isa::rti).retired == 1 && core.state().returned_frame,
          "The return-window fixture must expose the actual pending-source poll boundary");
  core.install(image, first_id);
  const auto compiled = core.statistics().compiled_entries;
  require(core.step().retired == 1 && !core.state().returned_frame &&
              !core.interrupt_frame_available() && core.statistics().compiled_entries == compiled,
          "The first prepared non-RTI instruction closes a preserved native return window");
}
}

void image_events() {
  const auto image = compile();
  constexpr std::array frames{Frame::push, Frame::waiting, Frame::returned};
  for (const auto frame : frames) {
    for (const auto request : interrupts) { event(image, frame, request); }
  }
  undeclared(image);
  wake(image, false);
  wake(image, true);
  returned_instruction(image);
}
}
