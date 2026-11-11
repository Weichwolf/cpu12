// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_model.hpp"
#include "image_support.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace cpu12::test::image {
namespace {
unsigned prefix(const unsigned deadline) {
  constexpr std::array<unsigned, 5> end_cycles{4, 7, 10, 13, 14};
  if (deadline == 0) { return 0; }
  for (std::size_t n = 0; n < end_cycles.size(); ++n) {
    if (deadline <= end_cycles[n]) { return static_cast<unsigned>(n + 1); }
  }
  return 5 + ((deadline - end_cycles.back() + 2) / 3);
}

void deadlines(const NativeImage &image) {
  Fixture fixture{Source(negative)};
  Core core(fixture.memory());
  core.install(image, first_id);
  const auto original = fixture.bytes;
  const auto compiled = core.statistics().compiled_entries;
  constexpr unsigned bound = 64;
  for (unsigned delta = 0; delta <= bound; ++delta) {
    fixture.bytes = original;
    core.set_registers(initial);
    const auto progress = core.progress().estimated_cycles;
    const auto count = prefix(delta);
    const auto expected = predict(initial, count, fixture);
    auto memory = original;
    if (count != 0) {
      memory[initial.sp - 2] = static_cast<Byte>(table >> byte_bits);
      memory[initial.sp - 1] = static_cast<Byte>(table);
    }
    const auto result = core.run_until(progress + delta);
    require(result.retired == count && result.fault == Fault::none &&
                core.state() == State{.registers = expected.registers, .failure = {}} &&
                core.progress() == Progress{.estimated_cycles = progress + expected.cycles} &&
                fixture.bytes == memory && core.statistics().compiled_entries == compiled,
            "Prepared deadline execution must preserve exact independent atomic prefixes and RAM");
  }
}

void deferral() {
  constexpr std::array<Byte, 4> code{0x10, 0xef, bra, backedge};
  constexpr std::array<Address, 1> entries{entry};
  const std::array spans{CodeSpan{.address = entry, .words = code}};
  const std::array variants{CodeVariant{.id = first_id, .code = spans, .entries = entries}};
  const std::array layouts{VariantLayout{.id = first_id, .memory = layout}};
  const auto image = Core::compile({.variants = variants}, {.memory = layouts, .interrupts = {}});
  Fixture fixture{Source(negative)};
  std::ranges::copy(code, fixture.bytes.begin() + entry);
  Core core(fixture.memory());
  core.install(image, first_id);
  auto before = initial;
  before.ccr = 0x10;
  core.set_registers(before);
  const auto compiled = core.statistics().compiled_entries;
  require(core.step().retired == 1 && core.state().irq_delay == 1 &&
              !core.interrupt_ready(InterruptKind::maskable),
          "A prepared CLI must retain the fresh one-instruction IRQ delay");
  const auto cycles = core.progress().estimated_cycles;
  constexpr unsigned deadline = 100;
  require(core.run_until(cycles + deadline).retired == 1 && core.state().irq_delay == 0 &&
              core.interrupt_ready(InterruptKind::maskable) &&
              core.progress().estimated_cycles == cycles + 3 &&
              core.statistics().compiled_entries == compiled,
          "Prepared execution must return at the exact IRQ-unmask boundary without lazy codegen");
}
}

void image_deadlines() {
  const auto image = compile();
  deadlines(image);
  deferral();
}
}
