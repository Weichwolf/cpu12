// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_frames.hpp"
#include "interrupt_model.hpp"

namespace cpu12::test::image_frames {
namespace {
namespace frames = cpu12::test::interrupts;

void deadline(
    Core &core, Fixture &fixture, const Form form, const Registers before, const unsigned delta) {
  auto target = frames::restored;
  target.pc = entry + 11;
  if (form.pc == rti) { frames::put(fixture.bytes, before.sp, frames::encode(target)); }
  auto memory = fixture.bytes;
  core.set_registers(before);
  const auto cycles = core.progress().estimated_cycles;
  const auto compiled = core.statistics().compiled_entries;
  auto wanted = before;
  State state{.registers = wanted, .failure = {}};
  unsigned retired = 0;
  unsigned cost = 0;
  if (delta != 0) {
    ++wanted.pc;
    cost = 1;
    retired = 1;
  }
  if (delta > 1) {
    ++retired;
    if (form.pc == rti) {
      wanted = frames::expected(wanted, target);
      state.returned_frame = true;
      cost += form.cycles;
    } else {
      constexpr unsigned stop_mask = 0x80;
      const bool masked = form.pc == stop && (wanted.ccr & stop_mask) != 0;
      auto saved = wanted;
      wanted = sleeping(wanted);
      saved.pc = wanted.pc;
      if (masked) {
        wanted.sp = before.sp;
        const auto loops = delta > 3 ? (delta - 3 + 2) / 3 : 0;
        retired += loops;
        cost += 2 + (loops * 3);
      } else {
        frames::put(memory, wanted.sp, frames::encode(saved));
        cost += form.cycles;
        state.execution = form.execution;
      }
    }
  }
  state.registers = wanted;
  const auto result = core.run_until(cycles + delta);
  require(result.retired == retired && result.fault == Fault::none && core.state() == state &&
              core.progress().estimated_cycles == cycles + cost && fixture.bytes == memory &&
              core.statistics().compiled_entries == compiled,
          "Prepared frame deadlines preserve exact atomic prefixes, cycles, masks, sleep and "
          "return boundaries");
}

void return_delay(Core &core, Fixture &fixture) {
  constexpr Address cli = entry + 14;
  auto before = seed;
  before.pc = cli;
  before.ccr |= 0x10;
  frames::put(fixture.bytes, before.sp, frames::encode(seed));
  core.set_registers(before);
  const auto cycles = core.progress().estimated_cycles;
  require(core.step().retired == 1 && core.state().irq_delay == 1,
          "A prepared CLI creates the genuine delayed IRQ window before RTI");
  require(core.run_until(cycles + 16).retired == 1 && core.state().irq_delay == 0 &&
              core.state().returned_frame && core.registers().pc == seed.pc &&
              core.progress().estimated_cycles == cycles + 9,
          "Prepared RTI clears a pre-existing CLI delay and returns at its source-poll boundary");
}
}

void deadlines(Core &core, Fixture &fixture) {
  for (const auto form : forms) {
    for (const Byte ccr : {Byte{0x20}, Byte{0xa0}}) {
      auto before = seed;
      before.pc = static_cast<Address>(form.pc - 1U);
      before.ccr = ccr;
      for (unsigned delta = 0; delta <= 16; ++delta) {
        deadline(core, fixture, form, before, delta);
      }
    }
  }
  return_delay(core, fixture);
}
}
