// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_transitions.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>

namespace cpu12::test::transitions {
namespace {
constexpr unsigned high_byte_mask = 0xff00, deadline_limit = 21;
enum class Boundary : Byte { quota, deadline };

void suspended(Core &core, Fixture &fixture) {
  const auto state = core.state();
  const auto progress = core.progress();
  const auto bytes = fixture.bytes;
  const auto known = fixture.known;
  const auto compiled = core.statistics().compiled_entries;
  for (unsigned repeat = 0; repeat < 3; ++repeat) {
    require(core.run(8).retired == 0 && core.step().retired == 0 &&
                core.run_until(progress.estimated_cycles + 32).retired == 0 &&
                core.run_instruction(isa::nop).retired == 0,
            "Every execution API waits at the real prepared transfer without replay");
    require(!core.interrupt_ready(InterruptKind::maskable) &&
                !core.interrupt_ready(InterruptKind::xirq) && !core.interrupt_frame_available() &&
                !core.interrupt(requests[0]).accepted && !core.resume_from_stop().accepted,
            "A suspended transfer cannot admit new events before its declared target is bound");
    rejects([&] { core.set_registers(seed); }, "pending CPU12 transition");
    rejects([&] { core.close_return_window(); }, "return boundary");
    const auto preparation = core.prepare(targets);
    require(preparation.failure.fault == Fault::unprepared_execution &&
                preparation.unresolved_entries == 1 && core.state() == state &&
                core.progress() == progress && fixture.bytes == bytes && fixture.known == known &&
                core.statistics().compiled_entries == compiled,
            "Pending preparation and rejected bypasses retain complete state, RAM, time and "
            "prepared code");
  }
}

void prefix(const NativeImage &image,
            const Byte flags,
            const Boundary boundary,
            const unsigned grant) {
  Fixture fixture;
  Core core(fixture.memory());
  core.install(image, from);
  auto initial = seed;
  initial.ccr = static_cast<Byte>(flags);
  core.set_registers(initial);
  auto bytes = fixture.bytes;
  auto known = fixture.known;
  const bool timed = boundary == Boundary::deadline;
  unsigned count = std::min(grant, 5U);
  if (timed) { count = grant <= 16 ? (grant + 3) / 4 : 5; }
  const auto writes = std::min(count, 4U);
  for (unsigned i = 0; i < writes; ++i) {
    bytes[destination + i] = payload[i];
    known[destination + i] = 1;
  }
  auto expected = initial;
  expected.pc = count == 5 ? destination : static_cast<Address>(start + (count * 5));
  const unsigned elapsed = count == 5 ? 19 : count * 4;
  const auto result = timed ? core.run_until(grant) : core.run(grant);
  require(result.retired == count && result.fault == Fault::none &&
              core.state() == State{.registers = expected, .failure = {}} &&
              core.progress() == Progress{.estimated_cycles = elapsed} && fixture.bytes == bytes &&
              fixture.known == known,
          "Real MOVB loader stores and its final JMP preserve every independently modeled "
          "instruction/deadline prefix");
  if (count != 5) { return; }
  suspended(core, fixture);
  const auto state = core.state();
  core.set_memory(fixture.memory(true));
  suspended(core, fixture);
  core.install(image, to);
  require(core.state() == state && core.progress().estimated_cycles == elapsed,
          "Selecting the target variant preserves the actual loader boundary");
  expected.pc = destination + 2;
  expected.d = static_cast<Address>((expected.d & high_byte_mask) | payload[1]);
  expected.ccr = static_cast<Byte>(expected.ccr & ~nzv);
  require(core.step().retired == 1 && core.state() == State{.registers = expected, .failure = {}} &&
              core.progress().estimated_cycles == elapsed + 1 && fixture.bytes == bytes &&
              fixture.known == known && core.statistics().compiled_entries == 0,
          "The prepared target executes its original first instruction without decoding, emission "
          "or loader replay");
}
}
}

int main() {
  try {
    using namespace cpu12::test::transitions;
    const auto image = compile();
    require(image.report().prepared_entries == 7 && image.report().unresolved_entries == 0 &&
                image.report().failure.fault == cpu12::Fault::none,
            "Cold preparation covers both actual executable views, including initially unknown "
            "target RAM");
    for (unsigned flags = 0; flags < 256; ++flags) {
      for (unsigned grant = 0; grant <= 8; ++grant) {
        prefix(image, static_cast<cpu12::Byte>(flags), Boundary::quota, grant);
      }
      for (unsigned deadline = 0; deadline <= deadline_limit; ++deadline) {
        prefix(image, static_cast<cpu12::Byte>(flags), Boundary::deadline, deadline);
      }
    }
    contracts();
    transfers();
    state_boundaries();
    std::cout << "Verified 7936 original-loader prefixes, suspended transitions, transactional "
                 "binding and real control/IRQ transfers.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
