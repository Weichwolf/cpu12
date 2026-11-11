// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "jit/llvm/image.hpp"
#include "jit/llvm/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address entry = 0x4000, handler = 0x4100, nested = 0x4200;
constexpr Address vector = 0xfff2, stack_top = 0x8000, output = 0x1234;
constexpr std::size_t frame_bytes = 9;
constexpr unsigned byte_bits = 8;
constexpr Byte mask_i = 0x10, mask_x = 0x40;
constexpr std::uint64_t variant = 1, infinite = std::numeric_limits<std::uint64_t>::max();
constexpr auto idle =
    isa::program(isa::opcode<isa::Opcode::bra_relative>(), std::array<Byte, 1>{0xfe});

void require(const bool passed, const char *message) {
  if (!passed) { throw std::runtime_error(message); }
}

template <std::size_t Count> constexpr auto handler_code() {
  std::array<Byte, Count> padding{};
  padding.fill(isa::nop.front());
  return isa::program(
      padding,
      isa::opcode<isa::Opcode::staa_extended>(),
      std::array<Byte, 2>{static_cast<Byte>(output >> byte_bits), static_cast<Byte>(output)},
      idle);
}

struct Fixture {
  std::array<Byte, idle.size()> foreground = idle;
  decltype(handler_code<2>()) ordinary = handler_code<2>();
  decltype(handler_code<10>()) external = handler_code<10>();
  std::array<Byte, 4> vectors{static_cast<Byte>(handler >> byte_bits),
                              static_cast<Byte>(handler),
                              static_cast<Byte>(nested >> byte_bits),
                              static_cast<Byte>(nested)};
  std::array<Byte, 2 * frame_bytes> stack{}, stack_known{};
  const std::array<CodeSpan, 3> spans{CodeSpan{.address = entry, .words = foreground},
                                      CodeSpan{.address = handler, .words = ordinary},
                                      CodeSpan{.address = nested, .words = external}};
  const std::array<Address, 3> entries{entry, handler, nested};
  const std::array<CodeVariant, 1> variants{{{.id = variant, .code = spans, .entries = entries}}};
  const std::array<InterruptRequest, 2> requests{
      InterruptRequest{.vector = vector},
      InterruptRequest{.vector = vector + 2, .kind = InterruptKind::xirq}};
  std::vector<Ram> regions{
      {.first = entry, .bytes = foreground, .writable = false},
      {.first = handler, .bytes = ordinary, .writable = false},
      {.first = nested, .bytes = external, .writable = false},
      {.first = vector, .bytes = vectors, .writable = false},
      {.first = static_cast<Address>(stack_top - stack.size()), .bytes = stack}};
  std::unique_ptr<detail::llvm19::ImageProgram> analysis;
  NativeImage image;

  [[nodiscard]] Memory memory() const { return Memory::mapped(regions); }

  void prepare() {
    std::vector<RegionLayout> bindings;
    bindings.reserve(regions.size());
    for (const auto &region : regions) {
      bindings.push_back({.first = region.first,
                          .size = region.bytes.size(),
                          .writable = region.writable,
                          .tracked = !region.known.empty()});
    }
    const std::array layouts{VariantLayout{.id = variant, .memory = bindings}};
    analysis =
        std::make_unique<detail::llvm19::ImageProgram>(CodeImage{.variants = variants},
                                                       layouts,
                                                       std::span<const std::byte>{},
                                                       requests,
                                                       std::span<const CodeTransition>{},
                                                       std::span<const CallbackBinding>{},
                                                       detail::llvm19::ArtifactOutput::semantic);
    image = Core::compile({.variants = variants},
                          {.memory = layouts,
                           .interrupts = requests,
                           .transitions = {},
                           .callbacks = {},
                           .bindings = {}});
  }

  [[nodiscard]] std::uint64_t prefix(const State &state) const {
    return analysis->interrupt_entry_prefix(variant, state, memory());
  }
};

struct Probe {
  Core *core = nullptr;
  std::uint64_t cycles = infinite;
  Address address = 0;

  bool observe(const Address at) noexcept {
    cycles = core->progress().estimated_cycles;
    address = at;
    return false;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    return static_cast<Probe *>(owner)->observe(access.address);
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    return static_cast<Probe *>(owner)->observe(access.address);
  }

  static bool read_word(void *owner, const Address address, Address &value) noexcept {
    value = 0;
    return static_cast<Probe *>(owner)->observe(address);
  }

  [[nodiscard]] Io io() {
    return {.owner = this, .write = write, .read_word = read_word, .write_word = write_word};
  }
};

enum class Frame : std::uint8_t { push, waiting, returned };

void seed(Core &core, const Frame frame, const Byte masks) {
  core.set_registers({.pc = entry, .sp = stack_top, .ccr = masks});
  if (frame == Frame::push) { return; }
  if (frame == Frame::waiting) {
    require(core.run_instruction(isa::wai).state == ExecutionState::waiting,
            "The fixture creates its frame with actual native WAI");
  } else {
    const auto kind = (masks & mask_i) == 0 ? InterruptKind::maskable : InterruptKind::xirq;
    const auto at = static_cast<Address>(vector + (kind == InterruptKind::xirq ? 2U : 0U));
    require(core.interrupt({.vector = at, .kind = kind}).accepted,
            "The fixture creates its saved frame with a real native interrupt entry");
    require(core.run_instruction(isa::rti).retired == 1 && core.state().returned_frame,
            "Actual native RTI opens the reusable return window");
  }
}

void direct_entries() {
  Fixture fixture;
  fixture.prepare();
  for (const auto frame : {Frame::push, Frame::waiting, Frame::returned}) {
    for (const auto kind : {InterruptKind::maskable, InterruptKind::xirq}) {
      Probe probe;
      auto io = probe.io();
      Core core(fixture.memory(), &io);
      probe.core = &core;
      seed(core, frame, kind == InterruptKind::maskable ? mask_x : mask_i);
      core.install(fixture.image, variant);
      const auto before = core.state();
      const auto cycles = core.progress().estimated_cycles;
      const auto stack = fixture.stack;
      const auto predicted = fixture.prefix(before);
      const std::uint64_t expected =
          (frame == Frame::push ? 9U : 6U) + (kind == InterruptKind::maskable ? 2U : 10U);
      require(
          predicted == expected && core.state() == before && fixture.stack == stack &&
              core.progress().estimated_cycles == cycles,
          "IRQ analysis neither executes nor changes frame/state/time and includes handler work");
      const auto event = core.interrupt(fixture.requests[kind == InterruptKind::maskable ? 0 : 1]);
      const auto result = core.run(16);
      require(event.accepted && event.fault == Fault::none && result.fault == Fault::memory_write &&
                  probe.address == output && probe.cycles - cycles == expected,
              "Real prepared push/WAI/RTI IRQ paths match the independent earliest-callback costs");
    }
  }
}

void tracked_frame() {
  Fixture fixture;
  fixture.regions.back().known = fixture.stack_known;
  fixture.prepare();
  Probe probe;
  auto io = probe.io();
  Core core(fixture.memory(), &io);
  probe.core = &core;
  seed(core, Frame::push, mask_x);
  core.install(fixture.image, variant);
  const auto known = fixture.stack_known;
  require(fixture.prefix(core.state()) == 11 && fixture.stack_known == known,
          "Unknown old stack values do not turn directly granted frame stores into host calls");
  require(core.interrupt(fixture.requests[0]).accepted && probe.cycles == infinite,
          "Actual prepared frame stores update tracked RAM without consulting the owner");
  for (std::size_t index = 0; index < fixture.stack_known.size(); ++index) {
    require(fixture.stack_known[index] == (index < frame_bytes ? 0 : 1),
            "Only the actual nine saved frame bytes become initialized");
  }
  require(core.run(16).fault == Fault::memory_write && probe.address == output &&
              probe.cycles == 11,
          "The first real owner call follows the complete tracked frame and two original NOPs");
}

void nested_stack_fault() {
  Fixture fixture;
  fixture.regions.back().first = stack_top - frame_bytes;
  fixture.regions.back().bytes = std::span(fixture.stack).last(frame_bytes);
  fixture.prepare();
  for (const auto frame : {Frame::push, Frame::waiting}) {
    Probe probe;
    auto io = probe.io();
    Core core(fixture.memory(), &io);
    probe.core = &core;
    seed(core, frame, 0);
    core.install(fixture.image, variant);
    const auto before = core.progress().estimated_cycles;
    const std::uint64_t expected = frame == Frame::push ? 9 : 6;
    require(fixture.prefix(core.state()) == expected,
            "A nested XIRQ may hit the stack owner before the original handler's first output");
    require(core.interrupt(fixture.requests[0]).accepted,
            "The first actual IRQ has a complete directly granted frame");
    const auto nested_event = core.interrupt(fixture.requests[1]);
    require(nested_event.fault == Fault::memory_write && !nested_event.accepted &&
                probe.cycles - before == expected && probe.address == stack_top - frame_bytes - 2,
            "Actual nested entry independently observes the predicted earlier stack callback");
  }
}

void vector_contracts() {
  constexpr std::size_t vector_region = 3;
  for (const bool split : {false, true}) {
    Fixture fixture;
    if (split) {
      fixture.regions[vector_region].bytes = std::span(fixture.vectors).first(1);
      fixture.regions.push_back(
          {.first = vector + 1, .bytes = std::span(fixture.vectors).subspan(1), .writable = false});
    } else {
      fixture.regions.erase(fixture.regions.begin() + vector_region);
    }
    fixture.prepare();
    Probe probe;
    auto io = probe.io();
    Core core(fixture.memory(), &io);
    probe.core = &core;
    seed(core, Frame::push, mask_x);
    core.install(fixture.image, variant);
    require(fixture.prefix(core.state()) == 0,
            "Unknown or split vector grants do not manufacture a direct word transaction");
    const auto event = core.interrupt(fixture.requests[0]);
    require(event.fault == Fault::memory_read && probe.address == vector && probe.cycles == 0,
            "Real prepared vector reads call the owner before any IRQ cycle is committed");
  }
  for (const bool alias : {false, true}) {
    Fixture fixture;
    if (alias) {
      constexpr Address mirror = 0x6000;
      fixture.regions.push_back({.first = mirror, .bytes = fixture.vectors});
    } else {
      fixture.regions[vector_region].writable = true;
    }
    fixture.prepare();
    require(fixture.prefix(
                {.registers = {.pc = entry, .sp = stack_top, .ccr = mask_x}, .failure = {}}) == 0,
            "A writable vector or physical writable alias cannot establish a stable IRQ target");
  }
}

void masks_and_deferral() {
  Fixture fixture;
  fixture.prepare();
  Core core(fixture.memory());
  seed(core, Frame::push, mask_i | mask_x);
  require(fixture.prefix(core.state()) == infinite,
          "Fully masked declared IRQs have no immediate output path");
  require(fixture.prefix({.registers = {.pc = entry, .sp = 0, .ccr = mask_x},
                          .failure = {},
                          .returned_frame = true}) == 8,
          "A reused wrapping RTI stack pointer retains its exact modular entry contract");
  constexpr Byte retain_x = 0xef;
  const auto clear_i =
      isa::program(isa::opcode<isa::Opcode::andcc_immediate>(), std::array{retain_x});
  require(core.run_instruction(clear_i).retired == 1,
          "Native CLI installs the architectural deferral");
  core.install(fixture.image, variant);
  require(core.state().irq_delay != 0 && !core.interrupt(fixture.requests[0]).accepted &&
              fixture.prefix(core.state()) == 11,
          "Remaining deferral is conservatively ignored in analysis, never bypassed by native "
          "admission");
}
}

int main() try {
  direct_entries();
  tracked_frame();
  nested_stack_fault();
  vector_contracts();
  masks_and_deferral();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
