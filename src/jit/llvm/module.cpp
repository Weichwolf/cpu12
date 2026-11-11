// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/module.hpp"

#include "cpu12/core.hpp"
#include "jit/llvm/abi.hpp"
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/callback_binding.hpp"
#include "jit/llvm/callbacks.hpp"
#include "jit/llvm/lifetime.hpp"
#include "jit/llvm/native.hpp"

#include "llvm-c/BitReader.h"
#include "llvm-c/Core.h"
#include "llvm-c/Error.h"
#include "llvm-c/LLJIT.h"
#include "llvm-c/Object.h"
#include "llvm-c/Orc.h"
#include "llvm-c/Target.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Types.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/Error.h"

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
#include <utility>
#include <vector>

namespace cpu12::detail::llvm19 {
namespace {
void checked(LLVMErrorRef error) {
  if (error == nullptr) { return; }
  const Owner<char *, LLVMDisposeErrorMessage> message(LLVMGetErrorMessage(error));
  throw std::runtime_error(message.get());
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
    const Owner<char *, LLVMDisposeErrorMessage> message(LLVMGetErrorMessage(error));
    std::cerr << message.get() << '\n';
    std::terminate();
  }
}

Owner<LLVMTargetMachineRef, LLVMDisposeTargetMachine> machine(LLVMModuleRef module) {
  const auto *triple = LLVMGetTarget(module);
  LLVMTargetRef target = nullptr;
  char *raw = nullptr;
  const auto failed = LLVMGetTargetFromTriple(triple, &target, &raw);
  const Owner<char *, LLVMDisposeMessage> message(raw);
  if (failed != 0) { throw std::runtime_error(message.get()); }
  const Owner<char *, LLVMDisposeMessage> cpu(LLVMGetHostCPUName());
  const auto features = native_features();
  Owner<LLVMTargetMachineRef, LLVMDisposeTargetMachine> result(
      LLVMCreateTargetMachine(target,
                              triple,
                              cpu.get(),
                              features.c_str(),
                              LLVMCodeGenLevelAggressive,
                              LLVMRelocPIC,
                              LLVMCodeModelSmall));
  if (!result) { throw std::runtime_error("Cannot create the CPU12 native materializer target"); }
  const Owner<LLVMTargetDataRef, LLVMDisposeTargetData> data(
      LLVMCreateTargetDataLayout(result.get()));
  const Owner<char *, LLVMDisposeMessage> layout(LLVMCopyStringRepOfTargetData(data.get()));
  if (std::string_view(layout.get()) != LLVMGetDataLayoutStr(module)) {
    throw std::invalid_argument("CPU12 semantic image has an incompatible native data layout");
  }
  return result;
}

void exports_valid(LLVMModuleRef module, const std::span<const SemanticExport> exports) {
  for (const auto entry : exports) {
    const std::string name(entry.name);
    auto *function = LLVMGetNamedFunction(module, name.c_str());
    if (function == nullptr || LLVMIsDeclaration(function) != 0 ||
        function_abi(function) != entry.abi) {
      throw std::invalid_argument("CPU12 native export is missing or has an incompatible ABI: " +
                                  name);
    }
  }
}

LLVMErrorRef measure(void *owner, LLVMMemoryBufferRef *buffer) noexcept {
  char *raw = nullptr;
  const Owner<LLVMBinaryRef, LLVMDisposeBinary> binary(LLVMCreateBinary(*buffer, nullptr, &raw));
  const Owner<char *, LLVMDisposeMessage> message(raw);
  if (!binary) {
    LLVMDisposeMemoryBuffer(*buffer);
    *buffer = nullptr;
    return LLVMCreateStringError(message.get());
  }
  const Owner<LLVMSectionIteratorRef, LLVMDisposeSectionIterator> sections(
      LLVMObjectFileCopySectionIterator(binary.get()));
  if (!sections) { return nullptr; }
  while (LLVMObjectFileIsSectionIteratorAtEnd(binary.get(), sections.get()) == 0) {
    const auto *raw_name = LLVMGetSectionName(sections.get());
    const std::string_view name(raw_name == nullptr ? "" : raw_name);
    if (name == "__text" || name == ".text" || name.starts_with(".text.") || name == ".ltext" ||
        name.starts_with(".ltext.")) {
      *static_cast<std::uint64_t *>(owner) += LLVMGetSectionSize(sections.get());
    }
    LLVMMoveToNextSection(sections.get());
  }
  return nullptr;
}
}

class NativeModule::Impl {
public:
  std::uint64_t native_bytes = 0;
  Owner<LLVMOrcLLJITRef, dispose> jit;
  std::map<std::string, const void *> addresses;
  std::vector<NativeSymbol> symbols;
};

NativeModule::NativeModule(const std::span<const std::byte> bitcode,
                           const std::span<const SemanticExport> exports,
                           const std::span<const std::byte> bindings,
                           const std::span<const CallbackBinding> callbacks)
    : impl_(std::make_unique<Impl>()) {
  weichwolf::llvm19::initialize_native();
  const Owner<LLVMContextRef, LLVMContextDispose> context(LLVMContextCreate());
  const Owner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> input(
      LLVMCreateMemoryBufferWithMemoryRangeCopy(
          reinterpret_cast<const char *>(bitcode.data()), bitcode.size(), "cpu12-image"));
  LLVMModuleRef raw_module = nullptr;
  const auto failed = LLVMParseBitcodeInContext2(context.get(), input.get(), &raw_module);
  const Owner<LLVMModuleRef, LLVMDisposeModule> module(raw_module);
  if (failed != 0 || !module) { throw std::invalid_argument("Invalid CPU12 semantic bitcode"); }
  exports_valid(module.get(), exports);
  link_bindings(module.get(), bindings, callbacks);
  require_callbacks(module.get(), callbacks);
  auto target = machine(module.get());
  optimize_module(module.get(), target.get());
  exports_valid(module.get(), exports);
  const auto object = llvm19::object(module.get(), target.get());
  Owner<LLVMOrcLLJITBuilderRef, LLVMOrcDisposeLLJITBuilder> builder(LLVMOrcCreateLLJITBuilder());
  LLVMOrcLLJITBuilderSetJITTargetMachineBuilder(
      builder.get(), LLVMOrcJITTargetMachineBuilderCreateFromTargetMachine(target.release()));
  LLVMOrcLLJITRef raw_jit = nullptr;
  checked(LLVMOrcCreateLLJIT(&raw_jit, builder.release()));
  impl_->jit.reset(raw_jit);
  LLVMOrcDefinitionGeneratorRef generator = nullptr;
  checked(LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
      &generator, LLVMOrcLLJITGetGlobalPrefix(impl_->jit.get()), nullptr, nullptr));
  LLVMOrcJITDylibAddGenerator(LLVMOrcLLJITGetMainJITDylib(impl_->jit.get()), generator);
  LLVMOrcObjectTransformLayerSetTransform(
      LLVMOrcLLJITGetObjTransformLayer(impl_->jit.get()), measure, &impl_->native_bytes);
  auto *buffer = LLVMCreateMemoryBufferWithMemoryRangeCopy(
      reinterpret_cast<const char *>(object.data()), object.size(), "cpu12-native");
  checked(LLVMOrcLLJITAddObjectFile(
      impl_->jit.get(), LLVMOrcLLJITGetMainJITDylib(impl_->jit.get()), buffer));
  for (const auto entry : exports) {
    LLVMOrcExecutorAddress address = 0;
    const std::string name(entry.name);
    checked(LLVMOrcLLJITLookup(impl_->jit.get(), &address, name.c_str()));
    impl_->addresses.emplace(name, std::bit_cast<const void *>(address));
  }
  auto &jit = native(impl_->jit.get());
  if (auto error = jit.initialize(jit.getMainJITDylib())) {
    throw std::runtime_error(llvm::toString(std::move(error)));
  }
  for (const auto &[name, address] : impl_->addresses) {
    impl_->symbols.push_back({.name = name, .address = address});
  }
}

NativeModule::~NativeModule() = default;

std::span<const NativeSymbol> NativeModule::symbols() const noexcept {
  return impl_->symbols;
}

std::uint64_t NativeModule::native_bytes() const noexcept {
  return impl_->native_bytes;
}
}
