// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "public_io.hpp"
#include "semantic_consumer.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::test::semantic {
namespace {
constexpr Address entry = 0x4000, output = 0x2345;
constexpr Address first_port = 0x1234, second_port = 0x1235;
constexpr Byte backedge = 0xfe;
constexpr std::uint64_t variant = 7;
constexpr unsigned byte_bits = 8, byte_mask = 255;
constexpr Byte retained_flags = 0xf1;
constexpr std::array callbacks{
    CallbackBinding{.role = CallbackRole::read, .symbol = "fixture_read"},
    CallbackBinding{.role = CallbackRole::write, .symbol = "fixture_write"}};

void require(const bool passed, const char *message) {
  if (!passed) { throw std::runtime_error(message); }
}

template <Address Input> consteval auto code() {
  return isa::program(
      isa::opcode<isa::Opcode::ldaa_extended>(),
      std::array<Byte, 2>{static_cast<Byte>(Input >> byte_bits), static_cast<Byte>(Input)},
      isa::opcode<isa::Opcode::staa_extended>(),
      std::array<Byte, 2>{static_cast<Byte>(output >> byte_bits), static_cast<Byte>(output)},
      isa::opcode<isa::Opcode::bra_relative>(),
      std::array<Byte, 1>{backedge});
}

template <Address Input> SemanticImage lower() {
  const auto bytes = code<Input>();
  const std::array source{CodeSpan{.address = entry, .words = bytes}};
  constexpr std::array roots{entry};
  const std::array variants{CodeVariant{.id = variant, .code = source, .entries = roots}};
  const std::array regions{RegionLayout{.first = entry, .size = bytes.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant, .memory = regions}};
  return Core::lower({.variants = variants},
                     {.memory = layouts,
                      .interrupts = {},
                      .transitions = {},
                      .callbacks = callbacks,
                      .bindings = {}});
}

std::vector<std::byte> host_ir() {
  LLVMMemoryBufferRef buffer = nullptr;
  char *message = nullptr;
  const auto failed =
      LLVMCreateMemoryBufferWithContentsOfFile(CPU12_PUBLIC_BINDINGS, &buffer, &message);
  if (message != nullptr) { LLVMDisposeMessage(message); }
  require(failed == 0 && buffer != nullptr, "Read independently compiled public-Io host bitcode");
  const auto *first = reinterpret_cast<const std::byte *>(LLVMGetBufferStart(buffer));
  std::vector<std::byte> result(first, first + LLVMGetBufferSize(buffer));
  LLVMDisposeMemoryBuffer(buffer);
  return result;
}

NativeImage bind(const SemanticImage &image, const std::shared_ptr<const Code> &code) {
  const auto symbols = code->symbols(image);
  return Core::bind(image,
                    {.owner = code,
                     .symbols = symbols,
                     .identity = image.identity(),
                     .native_bytes = code->bytes()});
}

void rejected(const SemanticImage &image, const NativeSymbols &symbols) {
  bool failed = false;
  try {
    static_cast<void>(Core::bind(image, symbols));
  } catch (const std::invalid_argument &) { failed = true; }
  require(failed, "Invalid image/native-owner receipts and export sets fail before installation");
}

void contracts(const SemanticImage &image, const std::shared_ptr<const Code> &code) {
  auto table = code->symbols(image);
  const NativeSymbols symbols{
      .owner = code, .symbols = table, .identity = image.identity(), .native_bytes = code->bytes()};
  const auto observed = calls();
  rejected({}, symbols);
  auto invalid = symbols;
  invalid.owner.reset();
  rejected(image, invalid);
  invalid = symbols;
  invalid.identity = "different-source-layout-pipeline-or-target";
  rejected(image, invalid);
  invalid = symbols;
  invalid.symbols = std::span(table).first(table.size() - 1);
  rejected(image, invalid);
  auto changed = table;
  changed.front().address = nullptr;
  invalid = symbols;
  invalid.symbols = changed;
  rejected(image, invalid);
  changed = table;
  changed.front().name = "wrong-export";
  invalid.symbols = changed;
  rejected(image, invalid);
  changed = table;
  changed.front().name = {};
  invalid.symbols = changed;
  rejected(image, invalid);
  changed = table;
  changed.back().name = changed.front().name;
  invalid.symbols = changed;
  rejected(image, invalid);
  require(calls() == observed,
          "Binding rejection creates no native machine/engine/object or lifecycle effect");
}

struct Observation {
  Core &core;
  const public_io::Bus &bus;
  std::array<Progress, 2> progress{};

  static void notify(void *owner) noexcept {
    auto &self = *static_cast<Observation *>(owner);
    if (self.bus.calls < self.progress.size()) {
      self.progress[self.bus.calls] = self.core.progress();
    }
  }
};

void execute(Core &core, public_io::Bus &bus, const Address input) {
  const auto observed = calls();
  Observation observation{.core = core, .bus = bus};
  bus.core = &core;
  bus.state = &core.state();
  bus.observer = &observation;
  bus.notify = Observation::notify;
  for (unsigned value = 0; value <= byte_mask; ++value) {
    for (const Byte flags : {Byte{0}, Byte{0xff}, Byte{0x55}, Byte{0x80}}) {
      const Registers before{
          .pc = entry, .d = 0x5aa5, .x = 0x1357, .y = 0x2468, .sp = 0x9000, .ccr = flags};
      core.set_registers(before);
      const auto ticks = core.progress().estimated_cycles;
      bus.memory[input] = static_cast<Byte>(value);
      bus.memory[output] = 0;
      bus.calls = 0;
      bus.trace = {};
      const auto result = core.run(3);
      auto wanted = before;
      wanted.pc = entry + 6;
      wanted.d = static_cast<Address>((value << byte_bits) | (before.d & byte_mask));
      wanted.ccr = static_cast<Byte>((flags & retained_flags) | (value >= 0x80 ? 8U : 0U) |
                                     (value == 0 ? 4U : 0U));
      auto store = wanted;
      store.pc = entry + 3;
      require(
          result.retired == 3 && result.fault == Fault::none &&
              core.state() == State{.registers = wanted, .failure = {}} &&
              core.progress() == Progress{.estimated_cycles = ticks + 9} && bus.calls == 2 &&
              bus.memory[output] == value &&
              bus.trace[0].state == State{.registers = before, .failure = {}} &&
              bus.trace[1].state == State{.registers = store, .failure = {}} &&
              bus.observed_state == &core.state() &&
              bus.trace[0].progress == Progress{.estimated_cycles = ticks} &&
              bus.trace[1].progress == Progress{.estimated_cycles = ticks + 3} &&
              observation.progress[0] == Progress{.estimated_cycles = ticks} &&
              observation.progress[1] == Progress{.estimated_cycles = ticks + 3},
          "Public bind executes exact native state/I/O/progress after all compiler inputs expire");
    }
  }
  require(calls() == observed,
          "Installed external artifacts execute without native compilation or reinitialization");
}

void resume(Core &core, public_io::Bus &bus, const Address input) {
  const auto before = calls();
  const Registers initial{.pc = entry, .d = 0x5aa5, .sp = 0x9000};
  core.set_registers(initial);
  const auto ticks = core.progress().estimated_cycles;
  Observation observation{.core = core, .bus = bus};
  bus.core = &core;
  bus.yield = true;
  bus.state = &core.state();
  bus.observer = &observation;
  bus.notify = Observation::notify;
  bus.memory[input] = 0x80;
  bus.memory[output] = 0;
  bus.calls = 0;
  bus.trace = {};
  const auto loaded = core.run(3);
  auto expected = initial;
  expected.pc = entry + 3;
  expected.d =
      static_cast<Address>((unsigned{bus.memory[input]} << byte_bits) | (initial.d & byte_mask));
  expected.ccr = 8;
  require(loaded.retired == 1 && loaded.fault == Fault::none && bus.calls == 1 &&
              bus.memory[output] == 0 &&
              core.state() == State{.registers = expected, .failure = {}} &&
              core.progress() == Progress{.estimated_cycles = ticks + 3},
          "A fused read yields after its exact committed prefix");
  const auto stored = core.run(2);
  expected.pc = entry + 6;
  require(stored.retired == 1 && stored.fault == Fault::none && bus.calls == 2 &&
              bus.memory[output] == 0x80 &&
              core.state() == State{.registers = expected, .failure = {}} &&
              core.progress() == Progress{.estimated_cycles = ticks + 6},
          "Resuming does not replay the read and preserves the write yield");
  bus.yield = false;
  const auto branched = core.run(1);
  require(branched.retired == 1 && branched.fault == Fault::none && bus.calls == 2 &&
              core.state() == State{.registers = expected, .failure = {}} &&
              core.progress() == Progress{.estimated_cycles = ticks + 9} && calls() == before,
          "Resuming the write retains native code and executes only the remaining branch");
}

void lifetime() {
  const auto before = calls();
  auto first = lower<first_port>();
  auto second = lower<second_port>();
  require(calls() == before && first.identity() != second.identity() && !first.exports().empty() &&
              !first.bitcode().empty(),
          "Lowering distinct images emits owned IR without native machinery");
  require(first.identity() == lower<first_port>().identity() && calls() == before,
          "Lowering identity depends on inputs/contracts/pipeline, never input buffer addresses");
  std::shared_ptr<const Code> owner;
  {
    const std::array components{first, second};
    owner = std::make_shared<const Code>(components, host_ir());
  }
  const auto prepared = calls();
  require(prepared.machines == before.machines + 1 && prepared.engines == before.engines + 1 &&
              prepared.objects == before.objects + 1 &&
              prepared.initialized == before.initialized + 1 &&
              prepared.finalized == before.finalized && owner->bytes() != 0,
          "One consumer optimizes/materializes both components once");
  contracts(first, owner);
  auto a = bind(first, owner);
  auto b = bind(second, owner);
  require(calls() == prepared && a.report().native_bytes == owner->bytes() &&
              b.report().native_bytes == owner->bytes(),
          "Both bindings retain one shared native resource");
  const std::weak_ptr<const Code> weak = owner;
  {
    auto first_code = code<first_port>();
    auto second_code = code<second_port>();
    public_io::Bus first_bus;
    public_io::Bus second_bus;
    Io first_io{.owner = &first_bus};
    Io second_io{.owner = &second_bus};
    Core first_core(Memory::window({.first = entry, .bytes = first_code, .writable = false}),
                    &first_io);
    Core second_core(Memory::window({.first = entry, .bytes = second_code, .writable = false}),
                     &second_io);
    first_core.set_registers({.pc = entry});
    second_core.set_registers({.pc = entry});
    first_core.install(a, variant);
    second_core.install(b, variant);
    a = {};
    b = {};
    first = {};
    second = {};
    owner.reset();
    require(!weak.expired() && calls() == prepared,
            "Either live core mapping retains shared code after handles expire");
    execute(first_core, first_bus, first_port);
    execute(second_core, second_bus, second_port);
    resume(first_core, first_bus, first_port);
    resume(second_core, second_bus, second_port);
  }
  require(weak.expired() && calls().finalized == before.finalized + 1,
          "The shared native resource finalizes exactly once after the last core releases it");
}

NativeImage compile(const std::span<const CallbackBinding> bindings,
                    const std::span<const std::byte> host) {
  const auto first = code<first_port>();
  const auto second = code<second_port>();
  const std::array first_source{CodeSpan{.address = entry, .words = first}};
  const std::array second_source{CodeSpan{.address = entry, .words = second}};
  constexpr std::array roots{entry};
  const std::array variants{
      CodeVariant{.id = variant, .code = first_source, .entries = roots},
      CodeVariant{.id = variant + 1, .code = second_source, .entries = roots}};
  const std::array regions{RegionLayout{.first = entry, .size = first.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant, .memory = regions},
                           VariantLayout{.id = variant + 1, .memory = regions}};
  return Core::compile({.variants = variants},
                       {.memory = layouts,
                        .interrupts = {},
                        .transitions = {},
                        .callbacks = bindings,
                        .bindings = host});
}

void standalone() {
  const auto host = host_ir();
  const auto before = calls();
  for (const auto *const wrong : {"fixture_write", "fixture_read_word", "fixture_missing"}) {
    const std::array bindings{CallbackBinding{.role = CallbackRole::read, .symbol = wrong}};
    bool rejected = false;
    try {
      static_cast<void>(compile(bindings, host));
    } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected && calls() == before,
            "Incompatible or absent callback definitions fail before native materialization");
  }
  bool rejected = false;
  try {
    static_cast<void>(compile(callbacks, {}));
  } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected && calls() == before,
          "Standalone selected callbacks require definitions before creating native code");
  const auto image = compile(callbacks, host);
  const auto prepared = calls();
  require(prepared.machines == before.machines + 1 && prepared.engines == before.engines + 1 &&
              prepared.objects == before.objects + 1 && image.report().native_bytes != 0 &&
              image.report().prepared_entries == 6 && image.report().unresolved_entries == 0,
          "Standalone compile materializes all lowered variants in one native owner");
  auto source = code<first_port>();
  public_io::Bus bus;
  Io io{.owner = &bus};
  Core core(Memory::window({.first = entry, .bytes = source, .writable = false}), &io);
  core.set_registers({.pc = entry});
  core.install(image, variant);
  execute(core, bus, first_port);
  resume(core, bus, first_port);
  Io unowned;
  Core nullable(Memory::window({.first = entry, .bytes = source, .writable = false}), &unowned);
  const Registers initial{.pc = entry, .sp = 0x9000};
  nullable.set_registers(initial);
  nullable.install(image, variant);
  const auto denied = nullable.run(1);
  require(denied.fault == Fault::memory_read && denied.retired == 0 &&
              denied.address == first_port && denied.value == 0 && denied.access_size == 1 &&
              nullable.registers() == initial && nullable.progress() == Progress{},
          "Nullable public callback owners reject a real access without undefined behavior or "
          "retirement");
}
}
}

int main() {
  try {
    cpu12::test::semantic::lifetime();
    cpu12::test::semantic::standalone();
    std::cout << "Verified public lower/bind, one independent native owner, exact I/O/state and "
                 "resource lifetime.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
