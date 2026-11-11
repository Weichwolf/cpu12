// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_transitions.hpp"
#include "interrupt_model.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace cpu12::test::transitions {
namespace {
void boundary(const Bytes instruction, const ExecutionState execution) {
  const bool waiting = execution != ExecutionState::running;
  const bool stopped = execution == ExecutionState::stopped;
  const auto entry = static_cast<Address>(destination - instruction.size());
  const std::array source{CodeSpan{.address = entry, .words = instruction}};
  const std::array target{CodeSpan{.address = destination, .words = payload}};
  const std::array entries{entry};
  const std::array variants{CodeVariant{.id = from, .code = source, .entries = entries},
                            CodeVariant{.id = to, .code = target, .entries = targets}};
  const std::array original{
      RegionLayout{.first = 0, .size = entry},
      RegionLayout{.first = entry, .size = instruction.size(), .writable = false},
      RegionLayout{.first = destination, .size = address_bytes - destination}};
  const std::array loaded{
      original[0],
      RegionLayout{.first = entry, .size = instruction.size() + payload.size(), .writable = false},
      RegionLayout{.first = destination + payload.size(),
                   .size = address_bytes - destination - payload.size()}};
  const std::array layouts{VariantLayout{.id = from, .memory = original},
                           VariantLayout{.id = to, .memory = loaded}};
  const auto image = Core::compile({.variants = variants},
                                   {.memory = layouts,
                                    .interrupts = requests,
                                    .transitions = forward,
                                    .callbacks = {},
                                    .bindings = {}});
  Fixture fixture(loop, payload, true);
  std::ranges::copy(instruction, fixture.bytes.begin() + entry);
  std::array<Ram, original.size()> grants{};
  const auto memory = [&](const std::span<const RegionLayout> layout) {
    for (std::size_t i = 0; i < grants.size(); ++i) {
      const auto &region = layout[i];
      grants[i] = {.first = region.first,
                   .bytes = std::span(fixture.bytes).subspan(region.first, region.size),
                   .writable = region.writable};
    }
    return Memory::mapped(grants);
  };
  Core core(memory(original));
  core.install(image, from);
  auto initial = seed;
  initial.pc = entry;
  initial.ccr = stopped ? 0x40 : 0x20;
  if (!waiting) { initial.ccr = 0x10; }
  core.set_registers(initial);
  auto expected = initial;
  expected.pc = destination;
  auto bytes = fixture.bytes;
  unsigned cycles = 1;
  if (waiting) { cycles = stopped ? 8 : 7; }
  if (waiting) {
    expected.sp -= 9;
    auto saved = initial;
    saved.pc = destination;
    test::interrupts::put(bytes, expected.sp, test::interrupts::encode(saved));
  } else {
    expected.ccr = 0;
  }
  const State wanted{.registers = expected,
                     .failure = {},
                     .execution = execution,
                     .irq_delay = static_cast<Byte>(waiting ? 0 : 1)};
  require(core.run(8).retired == 1 && core.state() == wanted &&
              core.progress().estimated_cycles == cycles && fixture.bytes == bytes &&
              core.run_until(cycles + 32).retired == 0 && !core.resume_from_stop().accepted,
          "A source fallthrough reaches its declared version boundary with exact WAI frame or "
          "fresh IRQ delay");
  core.set_memory(memory(loaded));
  core.install(image, to);
  require(core.state() == wanted && fixture.bytes == bytes &&
              core.statistics().compiled_entries == 0,
          "Binding a prepared fallthrough preserves hidden wait/deferral state and its original "
          "memory effects");
  if (stopped) {
    expected.sp += 9;
    require(core.resume_from_stop().accepted &&
                core.state() == State{.registers = expected, .failure = {}} &&
                core.progress().estimated_cycles == cycles && fixture.bytes == bytes,
            "STOP wake becomes admissible only after target binding and restores the actual "
            "stacked continuation");
  } else if (waiting) {
    expected.ccr |= 0x10;
    require(core.interrupt(requests[0]).accepted &&
                core.state() == State{.registers = expected, .failure = {}} &&
                core.progress().estimated_cycles == cycles + 6 && fixture.bytes == bytes,
            "The first real IRQ after binding reuses WAI's frame and performs only its actual "
            "vector entry");
  } else {
    require(!core.interrupt_ready(InterruptKind::maskable) && core.step().retired == 1 &&
                core.state().irq_delay == 0 && core.interrupt_ready(InterruptKind::maskable) &&
                core.progress().estimated_cycles == cycles + 1,
            "The target's first real instruction consumes the preserved CLI delay exactly once");
  }
}
}

void state_boundaries() {
  boundary(isa::wai, ExecutionState::waiting);
  boundary(isa::stop, ExecutionState::stopped);
  constexpr std::array<Byte, 2> cli{0x10, 0xef};
  boundary(cli, ExecutionState::running);
}
}
