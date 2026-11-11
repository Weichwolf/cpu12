// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_model.hpp"
#include "image_support.hpp"
#include "isa.hpp"

#include <array>
#include <span>

namespace cpu12::test::image {
namespace {
void unknown_entry(Core &core, Fixture &fixture, const Address pc) {
  auto seed = initial;
  seed.pc = pc;
  core.set_registers(seed);
  const auto progress = core.progress();
  const auto bytes = fixture.bytes;
  const auto compiled = core.statistics().compiled_entries;
  const auto result = core.run(1);
  require(
      result.fault == Fault::unprepared_execution && result.retired == 0 && result.pc == pc &&
          result.address == pc && core.registers() == seed && core.progress() == progress &&
          core.state().failure == result && fixture.bytes == bytes &&
          core.statistics().compiled_entries == compiled,
      "An undeclared PC must latch its exact failure without fetch, ISA execution or translation");
  require(core.step() == result && core.run(1) == result,
          "Strict image failures stay latched at the same architectural boundary");
}

void failed_installs(const NativeImage &image, Core &core, Fixture &fixture) {
  const auto compiled = core.statistics().compiled_entries;
  const auto before = core.state();
  const auto progress = core.progress();
  const auto bytes = fixture.bytes;
  rejects([&] { core.install(image, second_id); }, "executable source does not match");
  rejects([&] { core.install(image, first_id + 1); }, "Unknown CPU12 image variant");
  const NativeImage empty;
  rejects([&] { core.install(empty, first_id); }, "empty CPU12 image");
  require(core.state() == before && core.progress() == progress && fixture.bytes == bytes &&
              core.statistics().compiled_entries == compiled &&
              empty.report().prepared_entries == 0,
          "Rejected cold installations must retain the complete previous binding and state");
  core.set_registers({.pc = cases[0] + 2});
  require(core.run(1).retired == 1,
          "A rejected install cannot destroy a valid retained native binding");
}

void explicit_instruction(const NativeImage &image, Core &core, Fixture &fixture) {
  core.set_registers(initial);
  const auto state = core.state();
  const auto progress = core.progress();
  const auto bytes = fixture.bytes;
  const auto compiled = core.statistics().compiled_entries;
  constexpr auto nop = isa::program(isa::opcode<isa::Opcode::nop>());
  require(core.run_instruction(nop).fault == Fault::invalid_input && core.state() == state &&
              core.progress() == progress && fixture.bytes == bytes &&
              core.statistics().compiled_entries == compiled,
          "Explicit instruction input cannot replace installed original instruction bytes");
  const auto result = core.run_instruction(call_encoding);
  require(result.retired == 1 && result.fault == Fault::none && core.registers().pc == helper &&
              core.registers().sp == initial.sp - 2 &&
              core.statistics().compiled_entries == compiled,
          "Matching explicit input executes the prepared function without ISA emission");
  require(core.step().retired == 1 && core.registers().x == table &&
              core.statistics().compiled_entries == compiled,
          "Public step also executes the installed host code");
  const std::array seeds{entry, helper};
  const auto prepared = core.prepare(seeds);
  require(prepared.prepared_entries == image.report().prepared_entries / 2 &&
              prepared.native_bytes == 0 && prepared.failure.fault == Fault::none &&
              core.statistics().compiled_entries == compiled,
          "An installed image does not invoke the legacy graph compiler through prepare");
  const auto state_after = core.state();
  const auto progress_after = core.progress();
  const std::array<Address, 3> gaps{table, entry + 1, table};
  const auto missing = core.prepare(gaps);
  require(missing.unresolved_entries == 2 && missing.failure.pc == table &&
              missing.failure.fault == Fault::unprepared_execution && core.state() == state_after &&
              core.progress() == progress_after && core.statistics().compiled_entries == compiled,
          "Installed preparation reports unique missing entries and its first precise diagnostic "
          "without latching an execution fault or compiling code");
}

void remaps(const NativeImage &image, Core &core, Fixture &fixture) {
  core.set_memory(fixture.memory());
  unknown_entry(core, fixture, entry);
  core.set_registers(initial);
  require(core.prepare(std::array{entry}).failure.fault == Fault::unprepared_execution,
          "Prepare cannot implicitly re-enable translation after an installed image is remapped");
  require(core.run_instruction(call_encoding).fault == Fault::unprepared_execution,
          "Explicit execution cannot bypass an invalidated installation");
  core.set_registers(initial);
  core.install(image, first_id);
  require(core.run(1).retired == 1,
          "A matching stopped-owner reinstall recovers the native binding");
  Fixture changed{Source(0)};
  core.set_memory(changed.memory());
  core.set_registers(initial);
  rejects([&] { core.install(image, first_id); }, "executable source does not match");
  unknown_entry(core, changed, entry);
  core.set_registers({.pc = cases[0]});
  core.install(image, second_id);
  require(core.run(1).retired == 1 && core.registers().d == 0 && (core.registers().ccr & 4U) != 0,
          "Prepared variant selection uses its matching actual code version after a real remap");
  core.set_memory(fixture.memory());
  core.set_registers(initial);
  core.install(image, first_id);
}

void indirect_gap(const NativeImage &image) {
  Fixture fixture{Source(negative)};
  fixture.bytes[table] = 0;
  const auto original = fixture.bytes;
  Core core(fixture.memory());
  core.install(image, first_id);
  core.set_registers(initial);
  const auto compiled = core.statistics().compiled_entries;
  const auto result = core.run(8);
  auto expected = initial;
  expected.pc = table;
  expected.x = table;
  expected.ccr = 4;
  auto memory = original;
  memory[initial.sp - 2] = static_cast<Byte>(table >> byte_bits);
  memory[initial.sp - 1] = static_cast<Byte>(table);
  constexpr unsigned prefix_cycles = 13;
  require(
      result.fault == Fault::unprepared_execution && result.retired == 4 && result.pc == table &&
          result.address == table && core.registers() == expected &&
          core.progress() == Progress{.estimated_cycles = prefix_cycles} &&
          fixture.bytes == memory && core.state().failure.fault == Fault::unprepared_execution &&
          core.state().failure.retired == 0 && core.statistics().compiled_entries == compiled,
      "An actual indirect jump into undeclared table data must retain its complete native prefix "
      "and stop before new code is fetched, translated or executed");
  require(core.run(1).retired == 0 && core.progress().estimated_cycles == prefix_cycles,
          "A failed indirect entry cannot replay the preceding native prefix");
}
}

void image_contracts() {
  const auto image = compile();
  Fixture fixture{Source(negative)};
  Core core(fixture.memory());
  core.install(image, first_id);
  failed_installs(image, core, fixture);
  for (const auto pc : {table, static_cast<Address>(entry + 1), static_cast<Address>(code_end)}) {
    unknown_entry(core, fixture, pc);
  }
  explicit_instruction(image, core, fixture);
  remaps(image, core, fixture);
  indirect_gap(image);
  const Source source(negative);
  const std::array variants{source.variant(first_id)};
  const std::array layouts{VariantLayout{.id = first_id, .memory = layout}};
  const std::array duplicated{interrupts[0], interrupts[0]};
  rejects(
      [&] {
        static_cast<void>(Core::compile({.variants = variants},
                                        {.memory = layouts,
                                         .interrupts = duplicated,
                                         .transitions = {},
                                         .callbacks = {},
                                         .bindings = {}}));
      },
      "duplicate interrupt contracts");
  constexpr auto invalid = static_cast<InterruptKind>(0xff);
  constexpr std::array invalid_kind{InterruptRequest{.kind = invalid}};
  rejects(
      [&] {
        static_cast<void>(Core::compile({.variants = variants},
                                        {.memory = layouts,
                                         .interrupts = invalid_kind,
                                         .transitions = {},
                                         .callbacks = {},
                                         .bindings = {}}));
      },
      "invalid interrupt kind");
}
}
