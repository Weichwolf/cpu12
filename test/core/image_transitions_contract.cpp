// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_transitions.hpp"

#include <array>
#include <span>

namespace cpu12::test::transitions {
namespace {
void declarations() {
  rejects([] { static_cast<void>(compile(loader, payload, {})); }, "direct successor");
  for (const auto declaration :
       {CodeTransition{.from = 0, .to = to, .entry = destination},
        CodeTransition{.from = from, .to = 0, .entry = destination},
        CodeTransition{.from = from, .to = from, .entry = destination},
        CodeTransition{.from = from, .to = to, .entry = destination + 2}}) {
    const std::array transitions{declaration};
    rejects([&] { static_cast<void>(compile(loader, payload, transitions)); }, "transition");
  }
  constexpr std::array duplicated{forward[0], forward[0]};
  rejects([&] { static_cast<void>(compile(loader, payload, duplicated)); },
          "ambiguous code transition");
  constexpr std::array resident{CodeTransition{.from = from, .to = to, .entry = start}};
  const std::array first{CodeSpan{.address = start, .words = loop}};
  const std::array versions{CodeVariant{.id = from, .code = first, .entries = roots},
                            CodeVariant{.id = to, .code = first, .entries = roots}};
  rejects(
      [&] {
        static_cast<void>(Core::compile({.variants = versions},
                                        {.memory = {},
                                         .interrupts = {},
                                         .transitions = resident,
                                         .callbacks = {},
                                         .bindings = {}}));
      },
      "already resident");
}

void rejected_binding() {
  const auto image = compile();
  const auto foreign = compile();
  Fixture fixture;
  Core core(fixture.memory());
  core.install(image, from);
  core.set_registers(seed);
  require(core.run(32).retired == 5, "The binding fixture reaches its actual loader handoff");
  const auto before = core.state();
  const auto progress = core.progress();
  const auto memory = fixture.bytes;
  rejects([&] { core.install(image, from); }, "declared image and variant");
  rejects([&] { core.install(foreign, to); }, "declared image and variant");
  rejects([&] { core.install(image, to); }, "memory layout");
  core.set_memory(fixture.memory(true));
  fixture.known[destination] = 0;
  fixture.grants[3].known = std::span(fixture.known).subspan(destination, payload.size());
  core.set_memory(Memory::mapped(fixture.grants));
  rejects([&] { core.install(image, to); }, "memory layout");
  fixture.known[destination] = 1;
  core.set_memory(fixture.memory(true));
  fixture.bytes[destination] ^= 1;
  rejects([&] { core.install(image, to); }, "executable source does not match");
  fixture.bytes[destination] ^= 1;
  require(core.run(32).retired == 0 && core.state() == before && core.progress() == progress &&
              fixture.bytes == memory && core.statistics().compiled_entries == 0,
          "Rejected identities/layouts/unknown or changed words retain the complete suspended "
          "transfer");
  core.install(image, to);
  require(core.step().retired == 1 && core.registers().pc == destination + 2,
          "A corrected real target binding resumes the same pending original transfer");
}

void unknown_entry() {
  const auto image = compile();
  Fixture fixture(loader, payload, true);
  Core core(fixture.memory());
  core.install(image, from);
  auto initial = seed;
  initial.pc = destination;
  core.set_registers(initial);
  const auto bytes = fixture.bytes;
  require(core.run(0).retired == 0 && core.run(1).fault == Fault::unprepared_execution &&
              core.registers() == initial && core.progress() == Progress{} &&
              fixture.bytes == bytes,
          "Resident matching target bytes and an initial PC cannot invent a completed version "
          "transfer");
  core.set_registers(seed);
  require(
      core.run(32).retired == 5 && core.state().failure.fault == Fault::none,
      "An explicit register recovery can still execute the real loader after an ordinary fault");
}

void reset_ownership() {
  Fixture fixture;
  Core core(fixture.memory());
  {
    const auto image = compile();
    core.install(image, from);
    core.set_registers(seed);
    require(core.run(32).retired == 5, "The ownership fixture executes its actual loader");
  }
  core.set_memory(fixture.memory(true));
  const auto progress = core.progress();
  require(core.run(32).retired == 0 && core.state().failure.fault == Fault::none,
          "The pending target retains its native image after handles and previous mappings expire");
  const auto reset = core.reset(vector);
  require(reset.accepted && reset.fault == Fault::none && core.registers().pc == destination &&
              core.progress().estimated_cycles == progress.estimated_cycles + 5,
          "Only an actual reset transaction cancels pending transfer ownership and advances reset "
          "time");
  const auto independent = compile();
  core.install(independent, to);
  require(core.step().retired == 1 && core.registers().pc == destination + 2 &&
              core.statistics().compiled_entries == 0,
          "Reset permits a new explicitly installed prepared image without stale pending identity");
}
}

void contracts() {
  declarations();
  rejected_binding();
  unknown_entry();
  reset_ownership();
}
}
