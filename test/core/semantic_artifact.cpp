// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "semantic_consumer.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Error.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Transforms/PassBuilder.h"
#include "llvm-c/Types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
unsigned passes = 0;
unsigned builders = 0;

void require(const bool passed, const char *message) {
  if (!passed) { throw std::runtime_error(message); }
}

void rejected(const cpu12::CodeImage image,
              const cpu12::CompileOptions options,
              const std::span<const std::byte> bytes,
              const std::string_view reason) {
  const auto before = passes;
  const auto before_builders = builders;
  bool refused = false;
  try {
    static_cast<void>(cpu12::Core::load(image, options, bytes));
  } catch (const std::invalid_argument &error) {
    refused = std::string_view(error.what()).contains(reason);
  }
  require(refused && passes == before && builders == before_builders,
          "Stale/corrupt artifacts reject without LLVM lowering");
}
}

extern "C" LLVMBuilderRef LLVMCreateBuilderInContext(LLVMContextRef C) {
  ++builders;
  const auto *address = dlsym(RTLD_NEXT, "LLVMCreateBuilderInContext");
  require(address != nullptr, "Find packaged LLVM IR builder");
  return std::bit_cast<decltype(&LLVMCreateBuilderInContext)>(address)(C);
}

extern "C" LLVMErrorRef LLVMRunPasses(LLVMModuleRef M,
                                      const char *Passes,
                                      LLVMTargetMachineRef TM,
                                      LLVMPassBuilderOptionsRef Options) {
  ++passes;
  const auto *address = dlsym(RTLD_NEXT, "LLVMRunPasses");
  require(address != nullptr, "Find packaged LLVM pass runner");
  return std::bit_cast<decltype(&LLVMRunPasses)>(address)(M, Passes, TM, Options);
}

namespace cpu12::test::semantic {
SemanticImage
roundtrip(const CodeImage image, const CompileOptions options, SemanticImage producer) {
  require(builders != 0 && !Core::compiler_identity().empty() && !producer.artifact().empty(),
          "Cold lowering constructs LLVM IR and supplies a versioned artifact");
  LLVMMemoryBufferRef runtime = nullptr;
  char *message = nullptr;
  const auto failed =
      LLVMCreateMemoryBufferWithContentsOfFile(CPU12_RUNTIME_BITCODE, &runtime, &message);
  if (message != nullptr) { LLVMDisposeMessage(message); }
  require(failed == 0 && runtime != nullptr, "Read the actual packaged runtime IR input");
  const auto runtime_hash = llvm::SHA256::hash(
      llvm::ArrayRef(reinterpret_cast<const std::uint8_t *>(LLVMGetBufferStart(runtime)),
                     LLVMGetBufferSize(runtime)));
  LLVMDisposeMemoryBuffer(runtime);
  require(Core::compiler_identity().ends_with(llvm::toHex(llvm::ArrayRef(runtime_hash), true)),
          "Cheap compiler identity includes the exact host/build-specific runtime IR digest");
  const std::vector<std::byte> original(producer.artifact().begin(), producer.artifact().end());
  auto transport = original;
  const std::string identity(producer.identity());
  const std::vector<std::byte> bitcode(producer.bitcode().begin(), producer.bitcode().end());
  const auto exports = producer.exports().size();
  producer = {};
  const auto before = passes;
  const auto before_builders = builders;
  auto loaded = Core::load(image, options, transport);
  std::ranges::fill(transport, std::byte{0});
  require(passes == before && builders == before_builders && loaded.identity() == identity &&
              loaded.exports().size() == exports && std::ranges::equal(loaded.bitcode(), bitcode) &&
              std::ranges::equal(loaded.artifact(), original),
          "Restoration owns all IR/contracts after producer/transport expire, without lowering");
  for (const auto size :
       {std::size_t{0}, std::size_t{31}, original.size() / 2, original.size() - 1}) {
    rejected(image, options, std::span(original).first(size), "artifact");
  }
  const auto compiler = Core::compiler_identity();
  for (const auto offset : {std::size_t{0}, compiler.size() - 1}) {
    auto stale = original;
    const auto match = std::ranges::search(stale, std::as_bytes(std::span(compiler)));
    require(!match.empty(), "Artifact records the cheap compiler identity");
    match.begin()[static_cast<std::ptrdiff_t>(offset)] ^= std::byte{1};
    constexpr std::size_t digest_bytes = 32;
    const auto payload = std::span(stale).subspan(digest_bytes);
    const auto digest = llvm::SHA256::hash(
        llvm::ArrayRef(reinterpret_cast<const std::uint8_t *>(payload.data()), payload.size()));
    std::ranges::copy(std::as_bytes(std::span(digest)), stale.begin());
    rejected(image, options, stale, "compiler identity mismatch");
  }

  auto variants = std::vector(image.variants.begin(), image.variants.end());
  auto spans = std::vector(variants.front().code.begin(), variants.front().code.end());
  auto words = std::vector(spans.front().words.begin(), spans.front().words.end());
  words.front() ^= Byte{1};
  spans.front().words = words;
  variants.front().code = spans;
  rejected({.variants = variants}, options, original, "input identity mismatch");
  if (!options.memory.empty()) {
    auto layouts = std::vector(options.memory.begin(), options.memory.end());
    auto regions = std::vector(layouts.front().memory.begin(), layouts.front().memory.end());
    regions.front().tracked = !regions.front().tracked;
    layouts.front().memory = regions;
    auto changed = options;
    changed.memory = layouts;
    rejected(image, changed, original, "input identity mismatch");
  }
  auto changed = options;
  changed.topology =
      options.topology == MemoryTopology::fixed ? MemoryTopology::dynamic : MemoryTopology::fixed;
  rejected(image, changed, original, "input identity mismatch");
  return loaded;
}

void artifact_events(const std::span<const std::byte> host) {
  constexpr Address entry = 0x4000;
  constexpr Address handler = 0x5000;
  constexpr Address halted = 0x6000;
  constexpr Address stack = 0x8000;
  constexpr Address vector = 0xfff0;
  constexpr Address initial_sp = stack + 240;
  constexpr Address first_value = 0x5a34;
  constexpr Address second_value = 0x7c34;
  constexpr std::array<Byte, 2> spin{0x20, 0xfe};
  constexpr auto main = isa::program(isa::wai, isa::nop, spin);
  constexpr auto first_handler =
      isa::program(isa::opcode<isa::Opcode::ldaa_immediate>(), std::array<Byte, 1>{0x5a}, isa::rti);
  constexpr auto second_handler =
      isa::program(isa::opcode<isa::Opcode::ldaa_immediate>(), std::array<Byte, 1>{0x7c}, isa::rti);
  constexpr auto stopped = isa::program(isa::stop, spin);
  const std::array first{CodeSpan{.address = entry, .words = main},
                         CodeSpan{.address = handler, .words = first_handler},
                         CodeSpan{.address = halted, .words = stopped}};
  auto second = first;
  second[1].words = second_handler;
  constexpr std::array entries{entry, handler, halted};
  const std::array variants{CodeVariant{.id = 0, .code = first, .entries = entries},
                            CodeVariant{.id = 1, .code = second, .entries = entries}};
  const std::array layout{
      RegionLayout{.first = entry, .size = main.size(), .writable = false},
      RegionLayout{.first = handler, .size = first_handler.size(), .writable = false},
      RegionLayout{.first = halted, .size = stopped.size(), .writable = false},
      RegionLayout{.first = stack, .size = 256},
      RegionLayout{.first = vector, .size = 2, .writable = false}};
  const std::array layouts{VariantLayout{.id = 0, .memory = layout},
                           VariantLayout{.id = 1, .memory = layout}};
  const std::array requests{InterruptRequest{.vector = vector}};
  const CodeImage image{.variants = variants};
  const CompileOptions options{.memory = layouts,
                               .interrupts = requests,
                               .transitions = {},
                               .callbacks = {},
                               .bindings = {}};
  const auto semantic = roundtrip(image, options, Core::lower(image, options));
  const std::array components{semantic};
  const auto owner = std::make_shared<const Code>(components, host);
  const auto symbols = owner->symbols(semantic);
  const auto native = Core::bind(semantic,
                                 {.owner = owner,
                                  .symbols = symbols,
                                  .identity = semantic.identity(),
                                  .native_bytes = owner->bytes()});
  require(native.report().prepared_entries == 14,
          "Restored image retains both complete native variants");
  std::array<Byte, address_bytes> memory{};
  std::array<Ram, layout.size()> grants{};
  for (std::size_t index = 0; index < layout.size(); ++index) {
    const auto region = layout[index];
    grants[index] = {.first = region.first,
                     .bytes = std::span(memory).subspan(region.first, region.size),
                     .writable = region.writable};
  }
  memory[vector] = static_cast<Byte>(handler >> 8);
  memory[vector + 1] = static_cast<Byte>(handler);
  Core core(Memory::mapped(grants));
  for (const auto &variant : variants) {
    for (const auto span : variant.code) {
      std::ranges::copy(span.words, memory.begin() + span.address);
    }
    core.install(native, variant.id);
    const Registers initial{.pc = entry, .d = 0x1234, .sp = initial_sp};
    core.set_registers(initial);
    require(core.run(1).state == ExecutionState::waiting && core.registers().sp == initial_sp - 9,
            "Restored WAI writes its real stack frame");
    const auto ticks = core.progress().estimated_cycles;
    require(core.interrupt({.vector = vector}).accepted && core.registers().pc == handler &&
                core.progress().estimated_cycles == ticks + 6,
            "Restored event symbols admit the real six-cycle WAI vector entry");
    require(core.run(1).retired == 1 &&
                core.registers().d == (variant.id == 0 ? first_value : second_value),
            "Each restored variant executes its actual handler bytes");
    auto expected = initial;
    expected.pc = entry + 1;
    require(core.run(1).retired == 1 && core.registers() == expected && core.state().returned_frame,
            "Restored native RTI recovers the complete real frame");
    core.set_registers({.pc = halted, .sp = initial_sp, .ccr = 0x40});
    require(core.run(1).state == ExecutionState::stopped && core.resume_from_stop().accepted &&
                core.registers().pc == halted + 2,
            "Restored native STOP retains its ISA-specific wake continuation");
  }
  const auto before = core.save();
  for (const bool bad_code : {false, true}) {
    auto invalid = grants;
    if (bad_code) {
      memory[entry] ^= Byte{1};
    } else {
      invalid.back().writable = true;
    }
    core.set_memory(Memory::mapped(invalid));
    bool refused = false;
    try {
      core.install(native, 1);
    } catch (const std::invalid_argument &) { refused = true; }
    require(refused && core.save() == before,
            "Restored contracts still refuse actual code/layout mismatches without ISA effects");
    if (bad_code) { memory[entry] ^= Byte{1}; }
    core.set_memory(Memory::mapped(grants));
    core.install(native, 1);
    require(core.save() == before,
            "Rebinding validated storage preserves the complete processor state");
  }
}

}
