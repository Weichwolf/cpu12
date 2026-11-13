// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/program.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "immutable.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/callbacks.hpp"
#include "jit/llvm/events.hpp"
#include "jit/llvm/flow.hpp"
#include "jit/llvm/io_abi.hpp"
#include "jit/llvm/lifetime.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/memory.hpp"
#include "jit/llvm/native.hpp"
#include "jit/llvm/regions.hpp"
#include "jit/llvm/runtime_bitcode.hpp"
#include "jit/llvm/stack.hpp"
#include "jit/preparation.hpp"
#include "runtime.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Error.h"
#include "llvm-c/LLJIT.h"
#include "llvm-c/Object.h"
#include "llvm-c/Orc.h"
#include "llvm-c/Target.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpu12::detail::llvm19 {
namespace {
void require(LLVMErrorRef error) {
  if (error == nullptr) { return; }
  const Owner<char *, LLVMDisposeErrorMessage> message(LLVMGetErrorMessage(error));
  throw std::runtime_error(message.get());
}

void dispose_engine(LLVMOrcLLJITRef engine) noexcept {
  if (auto *error = LLVMOrcDisposeLLJIT(engine)) {
    const Owner<char *, LLVMDisposeErrorMessage> message(LLVMGetErrorMessage(error));
    std::cerr << message.get() << '\n';
    std::terminate();
  }
}

using Engine = Owner<LLVMOrcLLJITRef, dispose_engine>;

void bind_io(LLVMOrcLLJITRef engine) {
  const std::array bindings{
      std::pair{"cpu12_read_masked_byte", std::bit_cast<std::uintptr_t>(&cpu12_read_masked_byte)},
      std::pair{"cpu12_read_byte", std::bit_cast<std::uintptr_t>(&read_byte)},
      std::pair{"cpu12_read_word", std::bit_cast<std::uintptr_t>(&read_word)},
      std::pair{"cpu12_write_byte", std::bit_cast<std::uintptr_t>(&write_byte)},
      std::pair{"cpu12_write_word", std::bit_cast<std::uintptr_t>(&write_word)}};
  std::array<LLVMOrcCSymbolMapPair, bindings.size()> symbols{};
  for (std::size_t index = 0; index < bindings.size(); ++index) {
    symbols[index] = {.Name = LLVMOrcLLJITMangleAndIntern(engine, bindings[index].first),
                      .Sym = {.Address = bindings[index].second,
                              .Flags = {.GenericFlags = LLVMJITSymbolGenericFlagsExported |
                                                        LLVMJITSymbolGenericFlagsCallable,
                                        .TargetFlags = 0}}};
  }
  auto *unit = LLVMOrcAbsoluteSymbols(symbols.data(), symbols.size());
  if (auto *error = LLVMOrcJITDylibDefine(LLVMOrcLLJITGetMainJITDylib(engine), unit)) {
    LLVMOrcDisposeMaterializationUnit(unit);
    require(error);
  }
}

struct Layout {
  Address first;
  std::size_t size;
  bool writable, tracked;
  bool operator==(const Layout &) const = default;
};

std::vector<Layout> layout(const Memory &memory) {
  std::vector<Layout> result;
  for (const auto &region : memory.regions()) {
    result.push_back({.first = region.first,
                      .size = region.bytes.size(),
                      .writable = region.writable,
                      .tracked = !region.known.empty()});
  }
  return result;
}

struct Encoding {
  Address pc = 0;
  std::array<Byte, maximum_capture_bytes> bytes{};
  Byte length = 0;
  bool operator==(const Encoding &) const = default;
};

std::vector<Encoding> capture(const std::span<const BlockInstruction> region,
                              const Memory &memory) {
  std::vector<Encoding> result;
  result.reserve(region.size());
  for (const auto &operation : region) {
    Encoding source{.pc = operation.pc, .length = operation.instruction.length};
    if (source.length > source.bytes.size()) {
      throw std::logic_error("Prepared CPU12 code lost its immutable instruction bytes");
    }
    for (unsigned offset = 0; offset < source.length; ++offset) {
      const auto address = static_cast<Address>(source.pc + offset);
      const auto bytes = immutable_bytes(memory, address);
      if (bytes.empty()) {
        throw std::logic_error("Prepared CPU12 code lost its immutable instruction bytes");
      }
      source.bytes[offset] = bytes.front();
    }
    result.push_back(source);
  }
  return result;
}

bool matches(const std::span<const Encoding> source, const Memory &memory) {
  for (const auto &operation : source) {
    for (unsigned offset = 0; offset < operation.length; ++offset) {
      const auto address = static_cast<Address>(operation.pc + offset);
      const auto bytes = immutable_bytes(memory, address);
      if (bytes.empty() || bytes.front() != operation.bytes[offset]) { return false; }
    }
  }
  return true;
}

bool needs_bindings(const Instruction &instruction) {
  if (instruction.kind == Kind::effective_address) { return false; }
  if (instruction.kind == Kind::jump) { return instruction.indexed.indirect; }
  return memory_operand(instruction) || stack_operand(instruction) || frame_operand(instruction) ||
         instruction.kind == Kind::move;
}

Owner<LLVMTargetMachineRef, LLVMDisposeTargetMachine> target_machine(const ArtifactOutput output) {
  const Owner<char *, LLVMDisposeMessage> triple(LLVMGetDefaultTargetTriple());
  const Owner<char *, LLVMDisposeMessage> cpu(LLVMGetHostCPUName());
  const Owner<char *, LLVMDisposeMessage> features(LLVMGetHostCPUFeatures());
  const auto native =
      output != ArtifactOutput::none ? native_features() : std::string(features.get());
  LLVMTargetRef target = nullptr;
  char *raw = nullptr;
  const auto status = LLVMGetTargetFromTriple(triple.get(), &target, &raw);
  const Owner<char *, LLVMDisposeMessage> message(raw);
  if (status != 0) { throw std::runtime_error(message.get()); }
  Owner<LLVMTargetMachineRef, LLVMDisposeTargetMachine> machine(LLVMCreateTargetMachine(
      target,
      triple.get(),
      cpu.get(),
      native.c_str(),
      LLVMCodeGenLevelAggressive,
      LLVMRelocPIC,
      output != ArtifactOutput::none ? LLVMCodeModelSmall : LLVMCodeModelJITDefault));
  if (!machine) { throw std::runtime_error("Cannot create LLVM 19 native target machine"); }
  return machine;
}

TargetDescription description(LLVMTargetMachineRef machine) {
  if (machine == nullptr) { return runtime_target(); }
  const Owner<char *, LLVMDisposeMessage> triple(LLVMGetDefaultTargetTriple());
  const Owner<LLVMTargetDataRef, LLVMDisposeTargetData> data(LLVMCreateTargetDataLayout(machine));
  const Owner<char *, LLVMDisposeMessage> layout(LLVMCopyStringRepOfTargetData(data.get()));
  return {.triple = triple.get(), .layout = layout.get()};
}

LLVMErrorRef measure_object(void *owner, LLVMMemoryBufferRef *buffer) noexcept {
  char *raw = nullptr;
  const Owner<LLVMBinaryRef, LLVMDisposeBinary> binary(LLVMCreateBinary(*buffer, nullptr, &raw));
  const Owner<char *, LLVMDisposeMessage> message(raw);
  if (!binary) {
    LLVMDisposeMemoryBuffer(*buffer);
    *buffer = nullptr;
    return LLVMCreateStringError(message.get());
  }
  const Owner<LLVMSectionIteratorRef, LLVMDisposeSectionIterator> section(
      LLVMObjectFileCopySectionIterator(binary.get()));
  std::uint64_t bytes = 0;
  if (!section) { return nullptr; }
  while (LLVMObjectFileIsSectionIteratorAtEnd(binary.get(), section.get()) == 0) {
    const auto *section_name = LLVMGetSectionName(section.get());
    const std::string_view name(section_name == nullptr ? "" : section_name);
    if (name == "__text" || name == ".text" || name.starts_with(".text.") || name == ".ltext" ||
        name.starts_with(".ltext.")) {
      bytes += LLVMGetSectionSize(section.get());
    }
    LLVMMoveToNextSection(section.get());
  }
  *static_cast<std::uint64_t *>(owner) += bytes;
  return nullptr;
}
}

class Program::Impl {
public:
  struct Entry {
    const void *function;
    Instruction instruction;
  };

  std::vector<std::vector<BlockInstruction>>
  prepare(const std::span<const BlockInstruction> instructions, const ProgramMemory memory) {
    auto optimizing = regions(instructions);
    std::map<Address, Address> owners;
    for (const auto &region : optimizing) {
      for (const auto &operation : region) { owners.emplace(operation.pc, region.front().pc); }
    }
    for (const auto &region : optimizing) {
      sources.emplace(region.front().pc, capture(region, memory.source));
      const bool uses_memory = std::ranges::any_of(
          region, [](const auto &operation) { return needs_bindings(operation.instruction); });
      if (uses_memory) { layouts.emplace(region.front().pc, layout(memory.bindings)); }
      dependencies(region, owners);
    }
    return optimizing;
  }

  void dependencies(const std::span<const BlockInstruction> region,
                    const std::map<Address, Address> &owners) {
    const auto head = region.front().pc;
    for (const auto &operation : region) {
      const auto flow = successors(operation);
      const auto kind = operation.instruction.kind;
      if (flow.indirect && kind != Kind::wait && kind != Kind::stop &&
          kind != Kind::return_interrupt) {
        indirect.insert(head);
      }
      for (const auto address : std::span(flow.direct).first(flow.count)) {
        const auto found = owners.find(address);
        if (found != owners.end() && found->second != head) { targets[head].insert(found->second); }
      }
    }
  }

  [[nodiscard]] bool binding(const Address head, const Memory &memory) const {
    const auto found = layouts.find(head);
    return matches(sources.at(head), memory) &&
           (found == layouts.end() || found->second == layout(memory));
  }

  [[nodiscard]] bool dependencies(const Address head, const Memory &memory) const {
    std::set<Address> visited;
    std::vector<Address> pending{head};
    while (!pending.empty()) {
      const auto unit = pending.back();
      pending.pop_back();
      if (!visited.insert(unit).second) { continue; }
      if (!binding(unit, memory)) { return false; }
      if (indirect.contains(unit)) {
        return std::ranges::all_of(
            sources, [&](const auto &source) { return binding(source.first, memory); });
      }
      const auto found = targets.find(unit);
      if (found != targets.end()) {
        pending.insert(pending.end(), found->second.begin(), found->second.end());
      }
    }
    return true;
  }

  std::uint64_t native_bytes = 0;
  Engine engine;
  std::shared_ptr<const void> owner;
  std::map<Address, Entry> entries;
  std::map<std::uint64_t, const void *> events;
  std::map<std::uint64_t, std::string> event_symbols;
  std::map<Address, std::string> symbols;
  std::vector<std::byte> bitcode, object;
  std::map<Address, std::vector<Encoding>> sources;
  std::map<Address, std::vector<Layout>> layouts;
  std::map<Address, std::set<Address>> targets;
  std::set<Address> indirect;
};

Program::Program(const std::span<const BlockInstruction> instructions, const Memory &memory)
    : Program(instructions,
              {.memory = {.source = memory, .bindings = memory},
               .bindings = {},
               .interrupts = {},
               .callbacks = {}}) {}

Program::Program(const std::span<const BlockInstruction> instructions, const ProgramOptions options)
    : impl_(std::make_unique<Impl>()) {
  const auto functions = impl_->prepare(instructions, options.memory);
  if (functions.empty()) { return; }
  weichwolf::llvm19::initialize_native();
  Owner<LLVMTargetMachineRef, LLVMDisposeTargetMachine> machine;
  if (options.output != ArtifactOutput::semantic) { machine = target_machine(options.output); }
  const auto target = description(machine.get());
  Owner<LLVMOrcThreadSafeContextRef, LLVMOrcDisposeThreadSafeContext> thread_context(
      LLVMOrcCreateNewThreadSafeContext());
  auto *context = LLVMOrcThreadSafeContextGetContext(thread_context.get());
  Owner<LLVMModuleRef, LLVMDisposeModule> module(
      LLVMModuleCreateWithNameInContext("cpu12-prepared", context));
  LLVMSetDataLayout(module.get(), target.layout.c_str());
  LLVMSetTarget(module.get(), target.triple.c_str());
  if (options.dynamic_memory) {
    constexpr std::string_view key = "cpu12-dynamic-memory";
    LLVMAddModuleFlag(module.get(),
                      LLVMModuleFlagBehaviorError,
                      key.data(),
                      key.size(),
                      LLVMValueAsMetadata(LLVMConstInt(LLVMInt1TypeInContext(context), 1, 0)));
  }
  const auto name = [&](const auto &region) {
    return options.prefix + std::to_string(region.front().pc);
  };
  const Flow flow(module.get(),
                  functions,
                  options.memory.bindings,
                  options.prefix,
                  options.output != ArtifactOutput::none,
                  options.registers);
  impl_->event_symbols = options.prepare_events
                             ? lift_events(module.get(),
                                           options.memory.bindings,
                                           options.prefix,
                                           options.interrupts,
                                           options.output != ArtifactOutput::none)
                             : std::map<std::uint64_t, std::string>{};
  if (options.runtime_io || options.dynamic_memory || !options.callbacks.empty()) {
    link_bindings(module.get(), runtime_bitcode());
  }
  link_bindings(module.get(), options.bindings);
  link_callbacks(module.get(),
                 options.callbacks,
                 options.output == ArtifactOutput::semantic ? CallbackStage::declarations
                                                            : CallbackStage::definitions);
  if (options.output != ArtifactOutput::none) {
    for (const auto &region : functions) {
      for (const auto &operation : region) { impl_->symbols.emplace(operation.pc, name(region)); }
    }
    impl_->bitcode = llvm19::bitcode(module.get());
  }
  if (options.output == ArtifactOutput::semantic) { return; }
  optimize_module(module.get(), machine.get());
  if (options.output == ArtifactOutput::native) {
    impl_->object = llvm19::object(module.get(), machine.get());
  }
  Owner<LLVMOrcLLJITBuilderRef, LLVMOrcDisposeLLJITBuilder> builder(LLVMOrcCreateLLJITBuilder());
  LLVMOrcLLJITBuilderSetJITTargetMachineBuilder(
      builder.get(), LLVMOrcJITTargetMachineBuilderCreateFromTargetMachine(machine.release()));
  LLVMOrcLLJITRef raw_engine = nullptr;
  require(LLVMOrcCreateLLJIT(&raw_engine, builder.release()));
  impl_->engine.reset(raw_engine);
  bind_io(impl_->engine.get());
  LLVMOrcObjectTransformLayerSetTransform(
      LLVMOrcLLJITGetObjTransformLayer(impl_->engine.get()), measure_object, &impl_->native_bytes);
#if __has_feature(address_sanitizer)
  LLVMOrcDefinitionGeneratorRef generator = nullptr;
  require(LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
      &generator, LLVMOrcLLJITGetGlobalPrefix(impl_->engine.get()), nullptr, nullptr));
  LLVMOrcJITDylibAddGenerator(LLVMOrcLLJITGetMainJITDylib(impl_->engine.get()), generator);
#endif
  if (options.output == ArtifactOutput::native) {
    auto *buffer = LLVMCreateMemoryBufferWithMemoryRangeCopy(
        reinterpret_cast<const char *>(impl_->object.data()), impl_->object.size(), "cpu12-image");
    require(LLVMOrcLLJITAddObjectFile(
        impl_->engine.get(), LLVMOrcLLJITGetMainJITDylib(impl_->engine.get()), buffer));
  } else {
    Owner<LLVMOrcThreadSafeModuleRef, LLVMOrcDisposeThreadSafeModule> safe_module(
        LLVMOrcCreateNewThreadSafeModule(module.release(), thread_context.get()));
    thread_context.reset();
    require(LLVMOrcLLJITAddLLVMIRModule(impl_->engine.get(),
                                        LLVMOrcLLJITGetMainJITDylib(impl_->engine.get()),
                                        safe_module.release()));
  }
  for (const auto &region : functions) {
    LLVMOrcExecutorAddress symbol = 0;
    require(LLVMOrcLLJITLookup(impl_->engine.get(), &symbol, name(region).c_str()));
    for (const auto &operation : region) {
      impl_->entries.emplace(operation.pc,
                             Impl::Entry{.function = std::bit_cast<const void *>(symbol),
                                         .instruction = operation.instruction});
    }
  }
  for (const auto &[key, event_name] : impl_->event_symbols) {
    LLVMOrcExecutorAddress symbol = 0;
    require(LLVMOrcLLJITLookup(impl_->engine.get(), &symbol, event_name.c_str()));
    impl_->events.emplace(key, std::bit_cast<const void *>(symbol));
  }
}

Program::~Program() = default;

Program::Program(const Program &semantic,
                 const std::span<const BlockInstruction> instructions,
                 const NativeSymbols &symbols)
    : impl_(std::make_unique<Impl>()) {
  impl_->owner = symbols.owner;
  impl_->symbols = semantic.impl_->symbols;
  impl_->event_symbols = semantic.impl_->event_symbols;
  impl_->sources = semantic.impl_->sources;
  impl_->layouts = semantic.impl_->layouts;
  impl_->targets = semantic.impl_->targets;
  impl_->indirect = semantic.impl_->indirect;
  std::map<std::string, const void *> addresses;
  for (const auto symbol : symbols.symbols) { addresses.emplace(symbol.name, symbol.address); }
  for (const auto &operation : instructions) {
    impl_->entries.emplace(operation.pc,
                           Impl::Entry{.function = addresses.at(impl_->symbols.at(operation.pc)),
                                       .instruction = operation.instruction});
  }
  for (const auto &[key, name] : impl_->event_symbols) {
    impl_->events.emplace(key, addresses.at(name));
  }
}

const void *Program::entry(const Address address) const {
  return impl_->entries.at(address).function;
}

const void *Program::find(const Address address) const {
  const auto found = impl_->entries.find(address);
  return found != impl_->entries.end() ? found->second.function : nullptr;
}

const Instruction *Program::instruction(const Address address) const noexcept {
  const auto found = impl_->entries.find(address);
  return found != impl_->entries.end() ? &found->second.instruction : nullptr;
}

std::string_view Program::symbol(const Address address) const {
  return impl_->symbols.at(address);
}

std::span<const std::byte> Program::bitcode() const noexcept {
  return impl_->bitcode;
}

std::span<const std::byte> Program::object() const noexcept {
  return impl_->object;
}

const void *Program::event(const std::uint64_t key) const {
  const auto found = impl_->events.find(key);
  return found == impl_->events.end() ? nullptr : found->second;
}

std::string_view Program::event_symbol(const std::uint64_t key) const {
  return impl_->event_symbols.at(key);
}

const void *Program::find(const std::span<const BlockInstruction> region,
                          const Memory &memory) const {
  if (region.empty()) { return nullptr; }
  const auto found = impl_->sources.find(region.front().pc);
  if (found == impl_->sources.end() || found->second != capture(region, memory)) { return nullptr; }
  if (!impl_->dependencies(region.front().pc, memory)) { return nullptr; }
  return entry(region.front().pc);
}

std::uint64_t Program::native_bytes() const noexcept {
  return impl_->native_bytes;
}
}
