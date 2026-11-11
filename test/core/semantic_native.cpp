// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "semantic_consumer.hpp"

#include "llvm-c/Analysis.h"
#include "llvm-c/BitReader.h"
#include "llvm-c/Core.h"
#include "llvm-c/Error.h"
#include "llvm-c/LLJIT.h"
#include "llvm-c/Linker.h"
#include "llvm-c/Object.h"
#include "llvm-c/Orc.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Transforms/PassBuilder.h"
#include "llvm-c/Types.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/Error.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cpu12::test::semantic {
namespace {
constexpr std::array interfaces{"_ZNK5cpu124Core9registersEv",
                                "_ZNK5cpu124Core5stateEv",
                                "_ZNK5cpu124Core8progressEv",
                                "_ZN5cpu124Core13request_yieldEv"};

template <auto Dispose> struct Delete {
  void operator()(auto *value) const noexcept { Dispose(value); }
};
template <class Ref, auto Dispose>
using Owner = std::unique_ptr<std::remove_pointer_t<Ref>, Delete<Dispose>>;

void require(const bool passed, const char *message) {
  if (!passed) { throw std::runtime_error(message); }
}

void checked(LLVMErrorRef error) {
  if (error == nullptr) { return; }
  const Owner<char *, LLVMDisposeErrorMessage> message(LLVMGetErrorMessage(error));
  throw std::runtime_error(message.get());
}

void retained_interfaces(LLVMModuleRef module) {
  constexpr std::string_view attribute = "alwaysinline";
  const auto kind = LLVMGetEnumAttributeKindForName(attribute.data(), attribute.size());
  for (const auto *name : interfaces) {
    auto *function = LLVMGetNamedFunction(module, name);
    require(function != nullptr && LLVMIsDeclaration(function) == 0 &&
                LLVMGetLinkage(function) == LLVMWeakODRLinkage &&
                LLVMGetEnumAttributeAtIndex(
                    function, static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex), kind) !=
                    nullptr,
            "Detached public observer/yield bodies survive composition before host uses exist");
  }
}

void fused_interfaces(LLVMModuleRef module) {
  for (const auto *name : interfaces) {
    auto *function = LLVMGetNamedFunction(module, name);
    require(function == nullptr || LLVMGetFirstUse(function) == nullptr,
            "Final O3 fuses every public core observer/yield call into the host binding");
  }
}

llvm::orc::LLJIT &native(LLVMOrcLLJITRef jit) {
  return *reinterpret_cast<llvm::orc::LLJIT *>(jit);
}

void dispose(LLVMOrcLLJITRef jit) noexcept {
  if (auto error = native(jit).deinitialize(native(jit).getMainJITDylib())) {
    std::cerr << llvm::toString(std::move(error)) << '\n';
    std::terminate();
  }
  if (auto *error = LLVMOrcDisposeLLJIT(jit)) {
    LLVMConsumeError(error);
    std::terminate();
  }
  finalized();
}

void merge(LLVMModuleRef destination, const std::span<const std::byte> bytes) {
  const Owner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> buffer(
      LLVMCreateMemoryBufferWithMemoryRangeCopy(
          reinterpret_cast<const char *>(bytes.data()), bytes.size(), "input"));
  LLVMModuleRef raw = nullptr;
  require(LLVMParseBitcodeInContext2(LLVMGetModuleContext(destination), buffer.get(), &raw) == 0,
          "The public consumer can parse detached core/host IR");
  Owner<LLVMModuleRef, LLVMDisposeModule> module(raw);
  if (std::string_view(LLVMGetDataLayoutStr(destination)).empty()) {
    LLVMSetDataLayout(destination, LLVMGetDataLayoutStr(module.get()));
    LLVMSetTarget(destination, LLVMGetTarget(module.get()));
  }
  require(std::string_view(LLVMGetDataLayoutStr(module.get())).empty() ||
              std::string_view(LLVMGetDataLayoutStr(module.get())) ==
                  LLVMGetDataLayoutStr(destination),
          "Semantic and host inputs must have compatible native layouts");
  require(std::string_view(LLVMGetTarget(module.get())).empty() ||
              std::string_view(LLVMGetTarget(module.get())) == LLVMGetTarget(destination),
          "These native test inputs must target the same host");
  LLVMSetDataLayout(module.get(), LLVMGetDataLayoutStr(destination));
  LLVMSetTarget(module.get(), LLVMGetTarget(destination));
  require(LLVMLinkModules2(destination, module.release()) == 0,
          "Combine all semantic components before O3");
}

Owner<LLVMTargetMachineRef, LLVMDisposeTargetMachine> target(LLVMModuleRef module) {
  LLVMTargetRef architecture = nullptr;
  char *error = nullptr;
  const auto failed = LLVMGetTargetFromTriple(LLVMGetTarget(module), &architecture, &error);
  const Owner<char *, LLVMDisposeMessage> message(error);
  require(failed == 0, "Resolve the semantic artifact target");
  const Owner<char *, LLVMDisposeMessage> cpu(LLVMGetHostCPUName());
  const Owner<char *, LLVMDisposeMessage> features(LLVMGetHostCPUFeatures());
  return Owner<LLVMTargetMachineRef, LLVMDisposeTargetMachine>(
      LLVMCreateTargetMachine(architecture,
                              LLVMGetTarget(module),
                              cpu.get(),
                              features.get(),
                              LLVMCodeGenLevelAggressive,
                              LLVMRelocPIC,
                              LLVMCodeModelSmall));
}

void optimize(LLVMModuleRef module, LLVMTargetMachineRef machine) {
  const Owner<LLVMPassBuilderOptionsRef, LLVMDisposePassBuilderOptions> passes(
      LLVMCreatePassBuilderOptions());
#if __has_feature(address_sanitizer)
  constexpr std::string_view name = "sanitize_address";
  for (auto *function = LLVMGetFirstFunction(module); function != nullptr;
       function = LLVMGetNextFunction(function)) {
    if (LLVMIsDeclaration(function) == 0) {
      LLVMAddAttributeAtIndex(
          function,
          static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex),
          LLVMCreateEnumAttribute(LLVMGetModuleContext(module),
                                  LLVMGetEnumAttributeKindForName(name.data(), name.size()),
                                  0));
    }
  }
#endif
  checked(LLVMRunPasses(module, "default<O3>", machine, passes.get()));
#if __has_feature(address_sanitizer)
  checked(LLVMRunPasses(module, "asan", machine, passes.get()));
#endif
  require(LLVMVerifyModule(module, LLVMReturnStatusAction, nullptr) == 0,
          "The final joint module verifies");
}

void exports_valid(LLVMModuleRef module, const std::span<const SemanticImage> images) {
  for (const auto &image : images) {
    for (const auto entry : image.exports()) {
      const std::string name(entry.name);
      auto *function = LLVMGetNamedFunction(module, name.c_str());
      require(function != nullptr && LLVMIsDeclaration(function) == 0,
              "The consumer retains every guest/event definition before and after O3");
      const Owner<char *, LLVMDisposeMessage> type(
          LLVMPrintTypeToString(LLVMGlobalGetValueType(function)));
      const auto signature =
          "llvm19;cc=" + std::to_string(LLVMGetFunctionCallConv(function)) + ";type=" + type.get();
      require(entry.abi == signature,
              "These public guest/event exports preserve their exact unextended scalar ABI");
    }
  }
}

std::uint64_t text_bytes(LLVMMemoryBufferRef buffer) {
  char *error = nullptr;
  const Owner<LLVMBinaryRef, LLVMDisposeBinary> object(LLVMCreateBinary(buffer, nullptr, &error));
  const Owner<char *, LLVMDisposeMessage> message(error);
  require(object != nullptr, "The independent consumer emits a real native object");
  const Owner<LLVMSectionIteratorRef, LLVMDisposeSectionIterator> sections(
      LLVMObjectFileCopySectionIterator(object.get()));
  std::uint64_t bytes = 0;
  while (sections && LLVMObjectFileIsSectionIteratorAtEnd(object.get(), sections.get()) == 0) {
    const auto *raw = LLVMGetSectionName(sections.get());
    const std::string_view name(raw == nullptr ? "" : raw);
    if (name == "__text" || name == ".text" || name.starts_with(".text.")) {
      bytes += LLVMGetSectionSize(sections.get());
    }
    LLVMMoveToNextSection(sections.get());
  }
  return bytes;
}
}

class Code::Impl {
public:
  Owner<LLVMOrcLLJITRef, dispose> jit;
  std::map<std::string, const void *> symbols;
  std::uint64_t bytes = 0;
};

Code::Code(const std::span<const SemanticImage> images, const std::span<const std::byte> host)
    : impl_(std::make_unique<Impl>()) {
  const Owner<LLVMContextRef, LLVMContextDispose> context(LLVMContextCreate());
  const Owner<LLVMModuleRef, LLVMDisposeModule> module(
      LLVMModuleCreateWithNameInContext("consumer", context.get()));
  for (const auto &image : images) { merge(module.get(), image.bitcode()); }
  retained_interfaces(module.get());
  exports_valid(module.get(), images);
  merge(module.get(), host);
  auto machine = target(module.get());
  require(machine != nullptr, "Create the actual native optimizer target");
  optimize(module.get(), machine.get());
  fused_interfaces(module.get());
  exports_valid(module.get(), images);
  char *raw_error = nullptr;
  LLVMMemoryBufferRef raw_object = nullptr;
  const auto failed = LLVMTargetMachineEmitToMemoryBuffer(
      machine.get(), module.get(), LLVMObjectFile, &raw_error, &raw_object);
  const Owner<char *, LLVMDisposeMessage> error(raw_error);
  Owner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> object(raw_object);
  require(failed == 0 && object != nullptr, "Emit the jointly optimized host/core object");
  impl_->bytes = text_bytes(object.get());
  Owner<LLVMOrcLLJITBuilderRef, LLVMOrcDisposeLLJITBuilder> builder(LLVMOrcCreateLLJITBuilder());
  LLVMOrcLLJITBuilderSetJITTargetMachineBuilder(
      builder.get(), LLVMOrcJITTargetMachineBuilderCreateFromTargetMachine(machine.release()));
  LLVMOrcLLJITRef raw = nullptr;
  checked(LLVMOrcCreateLLJIT(&raw, builder.release()));
  impl_->jit.reset(raw);
  LLVMOrcDefinitionGeneratorRef generator = nullptr;
  checked(LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
      &generator, LLVMOrcLLJITGetGlobalPrefix(raw), nullptr, nullptr));
  LLVMOrcJITDylibAddGenerator(LLVMOrcLLJITGetMainJITDylib(raw), generator);
  checked(LLVMOrcLLJITAddObjectFile(raw, LLVMOrcLLJITGetMainJITDylib(raw), object.release()));
  for (const auto &image : images) {
    for (const auto entry : image.exports()) {
      LLVMOrcExecutorAddress address = 0;
      const std::string name(entry.name);
      checked(LLVMOrcLLJITLookup(raw, &address, name.c_str()));
      require(impl_->symbols.emplace(name, std::bit_cast<const void *>(address)).second,
              "Independent semantic images must not collide in the shared owner");
    }
  }
  auto &jit = native(raw);
  if (auto failure = jit.initialize(jit.getMainJITDylib())) {
    throw std::runtime_error(llvm::toString(std::move(failure)));
  }
  initialized();
}

Code::~Code() = default;

std::vector<NativeSymbol> Code::symbols(const SemanticImage &image) const {
  std::vector<NativeSymbol> result;
  for (const auto entry : image.exports()) {
    const auto found = impl_->symbols.find(std::string(entry.name));
    require(found != impl_->symbols.end(), "Every component export resolves from the same owner");
    result.push_back({.name = found->first, .address = found->second});
  }
  return result;
}

std::uint64_t Code::bytes() const noexcept {
  return impl_->bytes;
}
}
