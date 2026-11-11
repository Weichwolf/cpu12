// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_frames.hpp"
#include "image_support.hpp"
#include "interrupt_model.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace cpu12::test::image {
namespace {
namespace fixture = cpu12::test::image_frames;
namespace model = cpu12::test::interrupts;
constexpr unsigned byte_bits = 8, no_failure = 16;
constexpr unsigned irq_masks = 0x50;
constexpr Address handler = fixture::entry;
enum class Frame : std::uint8_t { push, waiting, returned };

struct Access {
  Address address = 0, value = 0;
  unsigned width = 0;
  bool write = false;
  bool operator==(const Access &) const = default;
};

struct Bus {
  fixture::Fixture &memory;
  Core *core = nullptr;
  State before;
  Progress progress;
  std::array<Access, 6> trace{};
  unsigned calls = 0, failure = no_failure, yielding = no_failure;
  bool audit = false, unchanged = true;

  bool observe(const Access access) noexcept {
    if (!audit) { return true; }
    const auto &state = core->state();
    unchanged &=
        state == before && core->progress() == progress && core->step().fault == Fault::reentrant;
    if (calls < trace.size()) { trace[calls] = access; }
    if (calls == yielding) { core->request_yield(); }
    return calls++ != failure;
  }

  static bool byte_read(void *owner, const Address at, Byte &value) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    value = self.memory.bytes[at];
    return self.observe({.address = at, .value = value, .width = 1});
  }

  static bool word_read(void *owner, const Address at, Address &value) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    value = static_cast<Address>((unsigned{self.memory.bytes[at]} << byte_bits) |
                                 self.memory.bytes[static_cast<Address>(at + 1U)]);
    return self.observe({.address = at, .value = value, .width = 2});
  }

  static bool byte_write(void *owner, const ByteWrite access) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    self.memory.bytes[access.address] = access.value;
    return self.observe(
        {.address = access.address, .value = access.value, .width = 1, .write = true});
  }

  static bool word_write(void *owner, const WordWrite access) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    self.memory.bytes[access.address] = static_cast<Byte>(access.value >> byte_bits);
    self.memory.bytes[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value);
    return self.observe(
        {.address = access.address, .value = access.value, .width = 2, .write = true});
  }

  Io io() {
    return {.owner = this,
            .read = byte_read,
            .write = byte_write,
            .read_word = word_read,
            .write_word = word_write};
  }
};

void seed(Core &core, fixture::Fixture &memory, const Frame frame, Registers initial) {
  initial.pc = frame == Frame::waiting ? fixture::wai : fixture::entry;
  if (frame == Frame::returned) {
    model::put(memory.bytes, initial.sp, model::encode(initial));
    initial.pc = fixture::rti;
  }
  core.set_registers(initial);
  if (frame != Frame::push) {
    require(core.step().retired == 1 && core.state().returned_frame == (frame == Frame::returned),
            "The IRQ fixture enters an actual prepared WAI/RTI boundary");
  }
  memory.bytes[fixture::vector] = static_cast<Byte>(handler >> byte_bits);
  memory.bytes[fixture::vector + 1] = static_cast<Byte>(handler);
}

std::array<Access, 6> sequence(const Registers state) {
  const auto at = [&](const int offset) { return static_cast<Address>(int{state.sp} + offset); };
  const auto swapped = static_cast<Address>((state.d >> byte_bits) | (state.d << byte_bits));
  return {{{.address = fixture::vector, .value = handler, .width = 2},
           {.address = at(-2), .value = state.pc, .width = 2, .write = true},
           {.address = at(-4), .value = state.y, .width = 2, .write = true},
           {.address = at(-6), .value = state.x, .width = 2, .write = true},
           {.address = at(-8), .value = swapped, .width = 2, .write = true},
           {.address = at(-9), .value = state.ccr, .width = 1, .write = true}}};
}

void apply(model::Image &memory, const std::span<const Access> accesses) {
  for (const auto access : accesses) {
    if (!access.write) { continue; }
    memory[access.address] =
        static_cast<Byte>(access.width == 1 ? access.value : access.value >> byte_bits);
    if (access.width == 2) {
      memory[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value);
    }
  }
}

void check(const NativeImage &image,
           const Frame frame,
           const Registers initial,
           const InterruptRequest request,
           const unsigned phase,
           const bool yielding) {
  fixture::Fixture memory(fixture::external);
  Bus bus{.memory = memory, .before = {}, .progress = {}};
  auto io = bus.io();
  Core core(memory.memory(), &io);
  bus.core = &core;
  const auto compiled = core.statistics().compiled_entries;
  const auto pristine = memory.bytes;
  core.install(image, fixture::variant_id);
  require(core.statistics().compiled_entries == compiled && memory.bytes == pristine,
          "Image installation binds already compiled IRQ/wake entries without new native code");
  seed(core, memory, frame, initial);
  bus.before = core.state();
  bus.progress = core.progress();
  bus.audit = true;
  bus.failure = yielding ? no_failure : phase;
  bus.yielding = yielding ? phase : no_failure;
  const auto accesses = sequence(bus.before.registers);
  const unsigned length = frame == Frame::push ? 6 : 1;
  const bool accepted = yielding || phase == length;
  const auto calls = accepted ? length : phase + 1;
  auto expected_memory = memory.bytes;
  apply(expected_memory, std::span(accesses).first(calls));
  const auto result = core.interrupt(request);
  require(
      bus.unchanged && bus.calls == calls && memory.bytes == expected_memory &&
          core.statistics().compiled_entries == compiled,
      "Each IRQ preserves original callback state, ordered real effects and cold code ownership");
  for (unsigned index = 0; index < calls; ++index) {
    require(bus.trace[index] == accesses[index],
            "IRQ vector and frame logical transactions retain their exact order");
  }
  if (accepted) {
    auto expected = bus.before.registers;
    expected.pc = handler;
    if (frame != Frame::waiting) { expected.sp = static_cast<Address>(expected.sp - 9); }
    expected.ccr =
        static_cast<Byte>(expected.ccr | (request.kind == InterruptKind::xirq ? irq_masks : 0x10));
    require(
        result.accepted && result.fault == Fault::none &&
            core.state() == State{.registers = expected, .failure = {}} &&
            core.progress().estimated_cycles ==
                bus.progress.estimated_cycles + (frame == Frame::push ? 9U : 6U),
        "A complete IRQ publishes its full original state only after every accepted transaction");
  } else {
    const auto failed = accesses[phase];
    const auto fault = phase == 0 ? Fault::memory_read : Fault::memory_write;
    require(!result.accepted && result.fault == fault && result.address == failed.address &&
                result.value == failed.value && result.access_size == failed.width &&
                core.registers() == bus.before.registers && core.progress() == bus.progress &&
                core.state().execution == bus.before.execution &&
                core.state().returned_frame == bus.before.returned_frame,
            "A refused IRQ field retains CPU/time and reports its exact real partial effect");
    require(core.interrupt(request).fault == fault && core.run(16).fault == fault &&
                bus.calls == calls,
            "Latched IRQ faults never replay already completed vector/frame transactions");
  }
}

void flag_cases(const NativeImage &image,
                const Frame frame,
                const InterruptRequest request,
                const Address stack) {
  for (unsigned flags = 0; flags < 256; ++flags) {
    if ((flags & irq_masks) != 0) { continue; }
    auto initial = fixture::seed;
    initial.sp = stack;
    initial.ccr = static_cast<Byte>(flags);
    const unsigned length = frame == Frame::push ? 6 : 1;
    for (unsigned phase = 0; phase <= length; ++phase) {
      check(image, frame, initial, request, phase, false);
      if (phase != length) { check(image, frame, initial, request, phase, true); }
    }
  }
}
}

void image_irq_bus() {
  const auto image = fixture::compile(fixture::external);
  constexpr std::array frames{Frame::push, Frame::waiting, Frame::returned};
  constexpr std::array<Address, 5> stacks{0, 1, 4, 9, 0xffff};
  for (const auto frame : frames) {
    for (const auto request : fixture::requests) {
      for (const auto stack : stacks) { flag_cases(image, frame, request, stack); }
    }
  }
}
}
