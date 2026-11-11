// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "isa.hpp"
#include "jit/image.hpp"
#include "jit/llvm/image.hpp"
#include "jit/llvm/program.hpp"
#include "jit/prefixes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace cpu12;
constexpr Address entry = 0x4000, output = 0x1234;
constexpr std::uint64_t variant = 7, infinite = std::numeric_limits<std::uint64_t>::max();
constexpr unsigned byte_bits = 8, byte_values = 256;
constexpr auto instructions = isa::program(isa::inca,
                                           isa::opcode<isa::Opcode::bne_relative>(),
                                           std::array<Byte, 1>{3},
                                           isa::nop,
                                           isa::nop,
                                           isa::nop,
                                           isa::opcode<isa::Opcode::staa_extended>(),
                                           std::array<Byte, 2>{0x12, 0x34},
                                           isa::opcode<isa::Opcode::bra_relative>(),
                                           std::array<Byte, 1>{0xfe});

void require(const bool passed, const char *message) {
  if (!passed) { throw std::runtime_error(message); }
}

struct Input {
  std::remove_cv_t<decltype(instructions)> bytes = instructions;
  std::array<CodeSpan, 1> code{{{.address = entry, .words = bytes}}};
  std::array<Address, 1> entries{entry};
  std::array<CodeVariant, 1> variants{{{.id = variant, .code = code, .entries = entries}}};

  [[nodiscard]] Memory memory() {
    return Memory::window({.first = entry, .bytes = bytes, .writable = false});
  }
};

void control_flow() {
  Input input;
  const std::array memory{
      RegionLayout{.first = entry, .size = instructions.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant, .memory = memory}};
  const auto before = input.bytes;
  const detail::llvm19::ImageProgram image({.variants = input.variants},
                                           layouts,
                                           {},
                                           {},
                                           {},
                                           {},
                                           detail::llvm19::ArtifactOutput::semantic);
  constexpr std::array<Address, 8> pcs{0, 1, 3, 4, 5, 6, 9, 10};
  constexpr std::array<std::uint64_t, 7> expected{4, 3, 3, 2, 1, 0, infinite};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    require(image.foreground_prefix(variant, static_cast<Address>(entry + pcs[index])) ==
                expected[index],
            "Shortest control-flow costs include the taken branch and retain closed pure cycles");
  }
  bool rejected = false;
  try {
    static_cast<void>(image.foreground_prefix(variant, entry + pcs.back()));
  } catch (const std::out_of_range &) { rejected = true; }
  require(rejected && input.bytes == before,
          "Cold prefix derivation changes no input and invents no entry for an extension byte");
}

struct Probe {
  Core *core = nullptr;
  std::uint64_t cycles = 0;
  Address address = 0;
  Byte value = 0;

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &self = *static_cast<Probe *>(owner);
    self.cycles = self.core->progress().estimated_cycles;
    self.address = access.address;
    self.value = access.value;
    return false;
  }
};

void native_prefixes() {
  Input input;
  const std::array memory{
      RegionLayout{.first = entry, .size = instructions.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant, .memory = memory}};
  const auto image = Core::compile(
      {.variants = input.variants},
      {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
  Probe probe;
  Io io{.owner = &probe, .write = Probe::write};
  Core core(input.memory(), &io);
  probe.core = &core;
  core.set_registers({.pc = entry});
  core.install(image, variant);
  for (unsigned value = 0; value < byte_values; ++value) {
    core.set_registers({.pc = entry, .d = static_cast<Address>(value << byte_bits)});
    const auto before = core.progress().estimated_cycles;
    const auto result = core.run(16);
    const auto incremented = static_cast<Byte>((value + 1) % byte_values);
    const std::uint64_t expected = incremented == 0 ? 5 : 4;
    require(result.fault == Fault::memory_write && result.pc == entry + 6 &&
                probe.address == output && probe.value == incremented &&
                probe.cycles - before == expected,
            "Actual native callback prefixes independently match both branch timing paths");
  }
}

void layouts_and_boundaries() {
  Input input;
  const detail::ImagePlan plan({.variants = input.variants});
  const auto code = plan.variants().front().instructions;
  std::array<Byte, 1> data{};
  std::array<Byte, 1> known{1};
  std::array memory{Ram{.first = entry, .bytes = input.bytes, .writable = false},
                    Ram{.first = output, .bytes = data}};
  const auto mapped = Memory::mapped(memory);
  const auto pure = detail::foreground_prefixes(code, mapped);
  require(pure.at(entry) == infinite && pure.at(entry + 6) == infinite,
          "A fixed private RAM write does not manufacture a host callback boundary");
  memory.back().known = known;
  require(detail::foreground_prefixes(code, Memory::mapped(memory)).at(entry + 6) == 0,
          "Tracked runtime initialization is not assumed from cold scratch values");
  memory.back().known = {};
  memory.back().writable = false;
  require(detail::foreground_prefixes(code, Memory::mapped(memory)).at(entry + 6) == 0,
          "A read-only destination retains its real host callback boundary");
  memory.back().writable = true;
  auto missing = code;
  missing.pop_back();
  require(detail::foreground_prefixes(missing, mapped).at(entry + 6) == 0,
          "A missing control-flow successor cannot authorize unknown execution");
  auto changed = code;
  changed.front().instruction.kind = detail::Kind::register_operation;
  changed.front().instruction.operation = detail::RegisterOp::ccr_and;
  require(detail::foreground_prefixes(changed, mapped).at(entry) == 0,
          "An interrupt-mask change ends the old foreground promise");
  changed.front().instruction.kind = detail::Kind::load;
  changed.front().instruction.target = detail::WordTarget::sp;
  require(detail::foreground_prefixes(changed, mapped).at(entry) == 0,
          "A new stack pointer ends the old interrupt-entry context");
  changed.front().instruction.kind = detail::Kind::effective_address;
  changed.front().instruction.target = detail::WordTarget::x;
  changed.front().instruction.indexed.base = detail::IndexBase::sp;
  changed.front().instruction.indexed.update = 1;
  require(detail::foreground_prefixes(changed, mapped).at(entry) == 0,
          "Implicit indexed stack updates also end the old entry context");
  changed.front().instruction.indexed.update = 0;
  changed.front().instruction.indexed.indirect = true;
  require(detail::foreground_prefixes(changed, mapped).at(entry) == 0,
          "An indirect pointer read is not assumed to be a pure address calculation");
}

void asynchronous_entry() {
  constexpr Address handler = 0x4100;
  constexpr Address vector = 0xfff4;
  constexpr Address stack = 0x7e00;
  constexpr Byte signal = 0xf1;
  constexpr auto loop =
      isa::program(isa::opcode<isa::Opcode::bra_relative>(), std::array<Byte, 1>{0xfe});
  constexpr auto response = isa::program(
      isa::opcode<isa::Opcode::staa_extended>(), std::array<Byte, 2>{0x12, 0x34}, loop);
  auto foreground = loop;
  auto interrupt = response;
  std::array<Byte, 2> destination{static_cast<Byte>(handler >> byte_bits),
                                  static_cast<Byte>(handler)};
  std::array<Byte, 512> frame{};
  const std::array code{CodeSpan{.address = entry, .words = foreground},
                        CodeSpan{.address = handler, .words = interrupt}};
  constexpr std::array entries{entry, handler};
  const std::array versions{CodeVariant{.id = variant, .code = code, .entries = entries}};
  const std::array regions{Ram{.first = entry, .bytes = foreground, .writable = false},
                           Ram{.first = handler, .bytes = interrupt, .writable = false},
                           Ram{.first = vector, .bytes = destination, .writable = false},
                           Ram{.first = stack, .bytes = frame}};
  const auto memory = Memory::mapped(regions);
  const detail::ImagePlan plan({.variants = versions});
  require(detail::foreground_prefixes(plan.variants().front().instructions, memory).at(entry) ==
              infinite,
          "The foreground loop itself contains no callback or context-changing instruction");
  const std::array layout{
      RegionLayout{.first = entry, .size = foreground.size(), .writable = false},
      RegionLayout{.first = handler, .size = interrupt.size(), .writable = false},
      RegionLayout{.first = vector, .size = destination.size(), .writable = false},
      RegionLayout{.first = stack, .size = frame.size()}};
  const std::array layouts{VariantLayout{.id = variant, .memory = layout}};
  constexpr std::array requests{InterruptRequest{.vector = vector, .kind = InterruptKind::xirq}};
  const auto image = Core::compile({.variants = versions},
                                   {.memory = layouts,
                                    .interrupts = requests,
                                    .transitions = {},
                                    .callbacks = {},
                                    .bindings = {}});
  Probe probe;
  Io io{.owner = &probe, .write = Probe::write};
  Core core(memory, &io);
  probe.core = &core;
  core.set_registers({.pc = entry,
                      .d = static_cast<Address>(signal << byte_bits),
                      .sp = static_cast<Address>(stack + frame.size() - 1)});
  core.install(image, variant);
  const auto accepted = core.interrupt(requests.front());
  const auto result = core.run(1);
  require(accepted.accepted && accepted.fault == Fault::none &&
              result.fault == Fault::memory_write && probe.address == output &&
              probe.value == signal && probe.cycles != infinite,
          "A real prepared IRQ can publish despite an infinite foreground prefix; it needs a "
          "separate horizon");
}
}

int main() try {
  control_flow();
  native_prefixes();
  layouts_and_boundaries();
  asynchronous_entry();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
