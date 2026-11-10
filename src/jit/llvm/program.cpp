// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/program.hpp"

#include "jit/block.hpp"
#include "jit/llvm/lifetime.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/native.hpp"

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

#include <bit>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>

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

void optimize(LLVMModuleRef module, LLVMTargetMachineRef machine, LLVMValueRef function) {
  const Owner<LLVMPassBuilderOptionsRef, LLVMDisposePassBuilderOptions> options(
      LLVMCreatePassBuilderOptions());
  LLVMPassBuilderOptionsSetVerifyEach(options.get(), 1);
#if __has_feature(address_sanitizer)
  constexpr std::string_view attribute = "sanitize_address";
  LLVMAddAttributeAtIndex(
      function,
      static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex),
      LLVMCreateEnumAttribute(LLVMGetModuleContext(module),
                              LLVMGetEnumAttributeKindForName(attribute.data(), attribute.size()),
                              0));
#else
  static_cast<void>(function);
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
  while (LLVMObjectFileIsSectionIteratorAtEnd(binary.get(), section.get()) == 0) {
    const std::string_view name(LLVMGetSectionName(section.get()));
    if (name == "__text" || name.starts_with(".text")) {
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
  const void *entry = nullptr;
};

Program::Program(const std::span<const BlockInstruction> instructions)
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
  auto *function = lift(module.get(), context, instructions);
  optimize(module.get(), machine.get(), function);
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
  LLVMOrcExecutorAddress symbol = 0;
  require(LLVMOrcLLJITLookup(impl_->engine.get(), &symbol, "cpu12_prepared"));
  impl_->entry = std::bit_cast<const void *>(symbol);
}

Program::~Program() = default;

const void *Program::entry() const noexcept {
  return impl_->entry;
}

std::uint64_t Program::native_bytes() const noexcept {
  return impl_->native_bytes;
}
}
