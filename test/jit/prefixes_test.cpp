// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "isa.hpp"
#include "jit/image.hpp"
#include "jit/llvm/image.hpp"
#include "jit/llvm/program.hpp"
#include "jit/prefixes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

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
  std::uint64_t cycles = 0, prefix = infinite;
  Address address = 0;
  Byte value = 0;

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &self = *static_cast<Probe *>(owner);
    self.cycles = self.core->progress().estimated_cycles;
    self.prefix = self.core->foreground_prefix();
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
  require(core.foreground_prefix() == 0 && core.event_prefix() == 0,
          "An uninstalled owner has no native event-prefix guarantee");
  core.install(image, variant);
  constexpr std::array<Address, 8> pcs{0, 1, 3, 4, 5, 6, 9, 10};
  constexpr std::array<std::uint64_t, 8> expected_prefixes{4, 3, 3, 2, 1, 0, infinite, 0};
  for (std::size_t index = 0; index < pcs.size(); ++index) {
    core.set_registers({.pc = static_cast<Address>(entry + pcs[index])});
    const auto state = core.state();
    const auto progress = core.progress();
    require(core.foreground_prefix() == expected_prefixes[index] && core.state() == state &&
                core.progress() == progress && probe.cycles == 0,
            "Public native prefixes preserve state and refuse undeclared instruction boundaries");
  }
  for (unsigned value = 0; value < byte_values; ++value) {
    core.set_registers({.pc = entry, .d = static_cast<Address>(value << byte_bits)});
    const auto before = core.progress().estimated_cycles;
    require(core.foreground_prefix() == 4 && core.progress().estimated_cycles == before,
            "A native prefix is conservative across both data-dependent branch outcomes");
    const auto result = core.run(16);
    const auto incremented = static_cast<Byte>((value + 1) % byte_values);
    const std::uint64_t expected = incremented == 0 ? 5 : 4;
    require(result.fault == Fault::memory_write && result.pc == entry + 6 &&
                probe.address == output && probe.value == incremented &&
                probe.cycles - before == expected && probe.prefix == 0,
            "Actual native callback prefixes independently match both branch timing paths");
    require(core.foreground_prefix() == 0 && core.event_prefix() == 0 &&
                core.state().failure.fault == Fault::memory_write,
            "A faulted owner cannot certify execution or clear its failure");
  }
}

void state_alias() {
  Input input;
  constexpr auto ccr = offsetof(State, registers) + offsetof(Registers, ccr);
  constexpr auto store = isa::program(isa::opcode<isa::Opcode::staa_extended>(),
                                      std::array<Byte, 2>{0, static_cast<Byte>(ccr)});
  std::ranges::copy(store, input.bytes.begin());
  const std::array layout{
      RegionLayout{.first = 0, .size = sizeof(State), .writable = true},
      RegionLayout{.first = entry, .size = instructions.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant, .memory = layout}};
  const auto image = Core::compile(
      {.variants = input.variants},
      {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
  Core core(input.memory());
  core.set_registers({.pc = entry});
  auto *state_bytes = std::bit_cast<Byte *>(const_cast<State *>(&core.state()));
  const std::array memory{Ram{.first = 0, .bytes = {state_bytes, sizeof(State)}},
                          Ram{.first = entry, .bytes = input.bytes, .writable = false}};
  const auto before = core.state();
  const auto progress = core.progress();
  core.set_memory(Memory::mapped(memory));
  core.install(image, variant);
  require(core.foreground_prefix() == 0 && core.event_prefix() == 0 && core.state() == before &&
              core.progress() == progress,
          "RAM that aliases hidden CPU state cannot certify a context-free prefix");
}

void waiting_prefix() {
  constexpr Address stack = 0x5000;
  constexpr Registers seed{.pc = entry, .d = 0x1234, .x = 0x5678, .y = 0x9abc, .ccr = 0x20};
  constexpr std::array<Byte, 9> frame{0x20, 0x34, 0x12, 0x56, 0x78, 0x9a, 0xbc, 0x40, 1};
  constexpr auto program =
      isa::program(isa::wai, isa::opcode<isa::Opcode::bra_relative>(), std::array<Byte, 1>{0xfe});
  enum class Layout : std::uint8_t { ordinary, tracked, readonly, split };
  for (const auto layout : {Layout::ordinary, Layout::tracked, Layout::readonly, Layout::split}) {
    auto bytes = program;
    std::array<Byte, 12> data{};
    std::array<Byte, 12> known{};
    const auto initial = data;
    std::vector<Ram> regions{{.first = entry, .bytes = bytes, .writable = false},
                             {.first = stack,
                              .bytes = data,
                              .known = layout == Layout::tracked ? known : std::span<Byte>{},
                              .writable = layout != Layout::readonly}};
    if (layout == Layout::split) {
      regions.back().bytes = std::span(data).first(data.size() / 2);
      regions.push_back(
          {.first = stack + (data.size() / 2), .bytes = std::span(data).last(data.size() / 2)});
    }
    std::vector<RegionLayout> grants;
    grants.reserve(regions.size());
    for (const auto &region : regions) {
      grants.push_back({.first = region.first,
                        .size = region.bytes.size(),
                        .writable = region.writable,
                        .tracked = !region.known.empty()});
    }
    const std::array code{CodeSpan{.address = entry, .words = bytes}};
    const std::array entries{entry};
    const std::array variants{CodeVariant{.id = variant, .code = code, .entries = entries}};
    const std::array layouts{VariantLayout{.id = variant, .memory = grants}};
    const auto image = Core::compile(
        {.variants = variants},
        {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
    Probe probe;
    Io io{.owner = &probe, .write = Probe::write};
    Core core(Memory::mapped(regions), &io);
    probe.core = &core;
    core.install(image, variant);
    for (const Address sp : {Address{4},
                             stack,
                             Address{stack + 8},
                             Address{stack + 9},
                             Address{stack + data.size()},
                             Address{stack + data.size() + 1}}) {
      auto registers = seed;
      registers.sp = sp;
      core.set_registers(registers);
      const auto state = core.state();
      const auto progress = core.progress();
      const bool admitted = (layout == Layout::ordinary || layout == Layout::tracked) &&
                            sp >= stack + frame.size() && sp <= stack + data.size();
      require(core.foreground_prefix() == (admitted ? 7U : 0U) && core.state() == state &&
                  core.progress() == progress && data == initial && known == initial &&
                  bytes == program && probe.prefix == infinite,
              "WAI admission requires its entire private writable frame without any execution");
    }
    if (layout == Layout::readonly || layout == Layout::split) { continue; }
    auto registers = seed;
    registers.sp = stack + data.size();
    core.set_registers(registers);
    const auto result = core.run_until(7);
    registers.pc += isa::wai.size();
    registers.sp -= frame.size();
    auto expected = initial;
    auto expected_known = initial;
    std::ranges::copy(frame, expected.end() - frame.size());
    if (layout == Layout::tracked) {
      std::fill(expected_known.end() - frame.size(), expected_known.end(), Byte{1});
    }
    require(result.fault == Fault::none && result.retired == 1 &&
                core.state().execution == ExecutionState::waiting &&
                core.registers() == registers && core.progress().estimated_cycles == 7 &&
                core.foreground_prefix() == 0 && probe.prefix == infinite && data == expected &&
                known == expected_known,
            "Native WAI reaches exactly its certified sleep boundary and writes the real frame");
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

void stack_counter() {
  constexpr Byte counter = 0x27;
  constexpr Byte self_branch = 0xfe;
  constexpr Address initial_stack = 3;
  auto bytes = isa::program(isa::opcode<isa::Opcode::dbne_relative>(),
                            std::array<Byte, 2>{counter, 0},
                            isa::opcode<isa::Opcode::bra_relative>(),
                            std::array<Byte, 1>{self_branch});
  const std::array code{CodeSpan{.address = entry, .words = bytes}};
  constexpr std::array entries{entry};
  const std::array versions{CodeVariant{.id = variant, .code = code, .entries = entries}};
  const auto memory = Memory::window({.first = entry, .bytes = bytes, .writable = false});
  const detail::ImagePlan plan({.variants = versions});
  require(detail::foreground_prefixes(plan.variants().front().instructions, memory).at(entry) == 0,
          "A stack-pointer loop counter ends the old interrupt-entry context");
  const std::array region{RegionLayout{.first = entry, .size = bytes.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant, .memory = region}};
  const auto image = Core::compile(
      {.variants = versions},
      {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
  Core core(memory);
  core.set_registers({.pc = entry, .sp = initial_stack});
  core.install(image, variant);
  const auto result = core.run(1);
  require(result.retired == 1 && result.fault == Fault::none &&
              core.registers().sp == initial_stack - 1 && core.registers().pc == entry + 3 &&
              core.progress().estimated_cycles == 3,
          "Actual native DBNE updates SP despite having no ordinary memory access");
}

struct Transfers {
  static constexpr std::size_t count = 198, stride = 7;
  std::array<Byte, count * stride> bytes{};
  std::array<Address, count> entries{};

  Transfers() {
    constexpr auto prototype = isa::program(isa::opcode<isa::Opcode::tfr>(),
                                            std::array<Byte, 1>{0},
                                            isa::opcode<isa::Opcode::staa_extended>(),
                                            std::array<Byte, 2>{0x12, 0x34},
                                            isa::opcode<isa::Opcode::bra_relative>(),
                                            std::array<Byte, 1>{0xfe});
    std::size_t index = 0;
    const auto append = [&](const Byte opcode, const Byte operand) {
      const auto offset = index * stride;
      std::ranges::copy(prototype, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
      bytes[offset] = opcode;
      bytes[offset + 1] = operand;
      entries[index++] = static_cast<Address>(entry + offset);
    };
    constexpr Byte transfer = 0xb7;
    constexpr Byte prefix = 0x18;
    constexpr unsigned selector_mask = 7;
    constexpr unsigned source_shift = 4;
    for (unsigned operand = 0; operand < byte_values; ++operand) {
      if (((operand >> source_shift) & selector_mask) == 3 || (operand & selector_mask) == 3) {
        continue;
      }
      append(transfer, static_cast<Byte>(operand));
    }
    append(prefix, 0x0e);
    append(prefix, 0x0f);
    require(index == count, "Exercise all TFR/EXG ignored-bit aliases and both flagged transfers");
  }
};

void transfer_prefixes() {
  Transfers input;
  const std::array code{CodeSpan{.address = entry, .words = input.bytes}};
  const std::array variants{CodeVariant{.id = variant, .code = code, .entries = input.entries}};
  const auto memory = Memory::window({.first = entry, .bytes = input.bytes, .writable = false});
  const detail::ImagePlan plan({.variants = variants});
  const auto prefixes = detail::foreground_prefixes(plan.variants().front().instructions, memory);
  constexpr std::size_t preserving = 122;
  std::size_t positive = 0;
  for (const auto pc : input.entries) {
    if (prefixes.at(pc) != 0) { ++positive; }
  }
  require(positive == preserving,
          "Seventy TFR aliases, fifty data-only EXG aliases and TAB/TBA preserve the context");
  const std::array region{
      RegionLayout{.first = entry, .size = input.bytes.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant, .memory = region}};
  const auto image = Core::compile(
      {.variants = variants},
      {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
  Probe probe;
  Io io{.owner = &probe, .write = Probe::write};
  Core core(memory, &io);
  probe.core = &core;
  core.set_registers({.pc = entry});
  core.install(image, variant);
  constexpr std::array seeds{Registers{.d = 0x7f80, .x = 0x1234, .y = 0xabcd, .sp = 0xfffe},
                             Registers{.d = 0xffff, .x = 0, .y = 0x1234, .sp = 0x7000},
                             Registers{.d = 0, .x = 0xffff, .y = 0x8000, .sp = 0}};
  constexpr Byte irq_masks = 0x50;
  for (const auto pc : input.entries) {
    const auto bound = prefixes.at(pc);
    const unsigned cycles = input.bytes[pc - entry] == 0x18 ? 2 : 1;
    require(bound == 0 || bound == cycles,
            "The bound reaches the next real owner write at its exact native transfer cost");
    for (auto seed : seeds) {
      seed.pc = pc;
      for (unsigned masks = 0; masks < byte_values; ++masks) {
        seed.ccr = static_cast<Byte>(masks);
        core.set_registers(seed);
        const auto before = core.progress().estimated_cycles;
        const auto result = core.run(16);
        const auto actual = core.registers();
        require(result.fault == Fault::memory_write && result.retired == 1 && result.pc == pc + 2 &&
                    probe.cycles - before == cycles && probe.address == output,
                "Actual native execution reaches precisely the predicted owner-write prefix");
        require(bound == 0 ||
                    (actual.sp == seed.sp && (actual.ccr & irq_masks) == (seed.ccr & irq_masks)),
                "Every positive bound preserves the actual stack and both IRQ admission masks");
      }
    }
  }
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
  state_alias();
  waiting_prefix();
  layouts_and_boundaries();
  stack_counter();
  transfer_prefixes();
  asynchronous_entry();
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
