// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/program.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "immutable.hpp"
#include "jit/block.hpp"
#include "jit/llvm/lifetime.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/native.hpp"
#include "jit/llvm/regions.hpp"

#include "llvm-c/Analysis.h"
#include "llvm-c/Core.h"
#include "llvm-c/Error.h"
#include "llvm-c/LLJIT.h"
#include "llvm-c/Object.h"
#include "llvm-c/Orc.h"
#include "llvm-c/Target.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Transforms/PassBuilder.h"
#include "llvm-c/Types.h"

#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cpu12::detail::llvm19 {
namespace {
void require(LLVMErrorRef error) {
  if (error == nullptr) { return; }
  const Owner<char *, LLVMDisposeErrorMessage> message(LLVMGetErrorMessage(error));
  throw std::runtime_error(message.get());
}

void verify(LLVMModuleRef module) {
  char *raw = nullptr;
  const auto status = LLVMVerifyModule(module, LLVMReturnStatusAction, &raw);
  const Owner<char *, LLVMDisposeMessage> message(raw);
  if (status != 0) { throw std::runtime_error(message.get()); }
}

void dispose_engine(LLVMOrcLLJITRef engine) noexcept {
  if (auto *error = LLVMOrcDisposeLLJIT(engine)) {
    const Owner<char *, LLVMDisposeErrorMessage> message(LLVMGetErrorMessage(error));
    std::cerr << message.get() << '\n';
    std::terminate();
  }
}

using Engine = Owner<LLVMOrcLLJITRef, dispose_engine>;

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

Owner<LLVMTargetMachineRef, LLVMDisposeTargetMachine> target_machine() {
  const Owner<char *, LLVMDisposeMessage> triple(LLVMGetDefaultTargetTriple());
  const Owner<char *, LLVMDisposeMessage> cpu(LLVMGetHostCPUName());
  const Owner<char *, LLVMDisposeMessage> features(LLVMGetHostCPUFeatures());
  LLVMTargetRef target = nullptr;
  char *raw = nullptr;
  const auto status = LLVMGetTargetFromTriple(triple.get(), &target, &raw);
  const Owner<char *, LLVMDisposeMessage> message(raw);
  if (status != 0) { throw std::runtime_error(message.get()); }
  Owner<LLVMTargetMachineRef, LLVMDisposeTargetMachine> machine(
      LLVMCreateTargetMachine(target,
                              triple.get(),
                              cpu.get(),
                              features.get(),
                              LLVMCodeGenLevelAggressive,
                              LLVMRelocPIC,
                              LLVMCodeModelJITDefault));
  if (!machine) { throw std::runtime_error("Cannot create LLVM 19 native target machine"); }
  return machine;
}

void optimize(LLVMModuleRef module, LLVMTargetMachineRef machine) {
  const Owner<LLVMPassBuilderOptionsRef, LLVMDisposePassBuilderOptions> options(
      LLVMCreatePassBuilderOptions());
#if __has_feature(address_sanitizer)
  constexpr std::string_view attribute = "sanitize_address";
  for (auto *function = LLVMGetFirstFunction(module); function != nullptr;
       function = LLVMGetNextFunction(function)) {
    if (LLVMIsDeclaration(function) == 0) {
      LLVMAddAttributeAtIndex(function,
                              static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex),
                              LLVMCreateEnumAttribute(LLVMGetModuleContext(module),
                                                      LLVMGetEnumAttributeKindForName(
                                                          attribute.data(), attribute.size()),
                                                      0));
    }
  }
#endif
  verify(module);
  require(LLVMRunPasses(module, "default<O3>", machine, options.get()));
#if __has_feature(address_sanitizer)
  require(LLVMRunPasses(module, "asan", machine, options.get()));
#endif
  verify(module);
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
  std::uint64_t native_bytes = 0;
  Engine engine;
  std::map<Address, const void *> entries;
  std::map<Address, std::vector<Encoding>> sources;
};

Program::Program(const std::span<const BlockInstruction> instructions, const Memory &memory)
    : impl_(std::make_unique<Impl>()) {
  weichwolf::llvm19::initialize_native();
  auto machine = target_machine();
  const Owner<char *, LLVMDisposeMessage> triple(LLVMGetDefaultTargetTriple());
  const Owner<LLVMTargetDataRef, LLVMDisposeTargetData> target_data(
      LLVMCreateTargetDataLayout(machine.get()));
  const Owner<char *, LLVMDisposeMessage> layout(LLVMCopyStringRepOfTargetData(target_data.get()));
  Owner<LLVMOrcThreadSafeContextRef, LLVMOrcDisposeThreadSafeContext> thread_context(
      LLVMOrcCreateNewThreadSafeContext());
  auto *context = LLVMOrcThreadSafeContextGetContext(thread_context.get());
  Owner<LLVMModuleRef, LLVMDisposeModule> module(
      LLVMModuleCreateWithNameInContext("cpu12-prepared", context));
  LLVMSetDataLayout(module.get(), layout.get());
  LLVMSetTarget(module.get(), triple.get());
  const auto functions = regions(instructions);
  const auto name = [](const auto &region) {
    return "cpu12_region_" + std::to_string(region.front().pc);
  };
  for (const auto &region : functions) {
    impl_->sources.emplace(region.front().pc, capture(region, memory));
    static_cast<void>(lift(module.get(), context, region, name(region).c_str()));
  }
  optimize(module.get(), machine.get());
  Owner<LLVMOrcLLJITBuilderRef, LLVMOrcDisposeLLJITBuilder> builder(LLVMOrcCreateLLJITBuilder());
  LLVMOrcLLJITBuilderSetJITTargetMachineBuilder(
      builder.get(), LLVMOrcJITTargetMachineBuilderCreateFromTargetMachine(machine.release()));
  LLVMOrcLLJITRef raw_engine = nullptr;
  require(LLVMOrcCreateLLJIT(&raw_engine, builder.release()));
  impl_->engine.reset(raw_engine);
  LLVMOrcObjectTransformLayerSetTransform(
      LLVMOrcLLJITGetObjTransformLayer(impl_->engine.get()), measure_object, &impl_->native_bytes);
#if __has_feature(address_sanitizer)
  LLVMOrcDefinitionGeneratorRef generator = nullptr;
  require(LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
      &generator, LLVMOrcLLJITGetGlobalPrefix(impl_->engine.get()), nullptr, nullptr));
  LLVMOrcJITDylibAddGenerator(LLVMOrcLLJITGetMainJITDylib(impl_->engine.get()), generator);
#endif
  Owner<LLVMOrcThreadSafeModuleRef, LLVMOrcDisposeThreadSafeModule> safe_module(
      LLVMOrcCreateNewThreadSafeModule(module.release(), thread_context.get()));
  thread_context.reset();
  require(LLVMOrcLLJITAddLLVMIRModule(impl_->engine.get(),
                                      LLVMOrcLLJITGetMainJITDylib(impl_->engine.get()),
                                      safe_module.release()));
  for (const auto &region : functions) {
    LLVMOrcExecutorAddress symbol = 0;
    require(LLVMOrcLLJITLookup(impl_->engine.get(), &symbol, name(region).c_str()));
    for (const auto &operation : region) {
      impl_->entries.emplace(operation.pc, std::bit_cast<const void *>(symbol));
    }
  }
}

Program::~Program() = default;

const void *Program::entry(const Address address) const {
  return impl_->entries.at(address);
}

const void *Program::find(const std::span<const BlockInstruction> region,
                          const Memory &memory) const {
  if (region.empty()) { return nullptr; }
  const auto found = impl_->sources.find(region.front().pc);
  if (found == impl_->sources.end() || found->second != capture(region, memory)) { return nullptr; }
  return entry(region.front().pc);
}

std::uint64_t Program::native_bytes() const noexcept {
  return impl_->native_bytes;
}
}
