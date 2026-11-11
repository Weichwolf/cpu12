// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/artifact.hpp"

#include "jit/llvm/lifetime.hpp"

#include "llvm-c/Analysis.h"
#include "llvm-c/BitReader.h"
#include "llvm-c/BitWriter.h"
#include "llvm-c/Core.h"
#include "llvm-c/Linker.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Types.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/TargetParser/AArch64TargetParser.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cpu12::detail::llvm19 {
namespace {
class DiagnosticScope {
public:
  explicit DiagnosticScope(LLVMContextRef context)
      : context_(context),
        handler_(LLVMContextGetDiagnosticHandler(context)),
        owner_(LLVMContextGetDiagnosticContext(context)) {
    LLVMContextSetDiagnosticHandler(context_, receive, this);
  }

  ~DiagnosticScope() { LLVMContextSetDiagnosticHandler(context_, handler_, owner_); }

  DiagnosticScope(const DiagnosticScope &) = delete;
  DiagnosticScope &operator=(const DiagnosticScope &) = delete;

  void check(const bool failed) const {
    if (exception_) { std::rethrow_exception(exception_); }
    if (failed || !message_.empty()) {
      throw std::invalid_argument(message_.empty() ? "Cannot link CPU12 binding bitcode"
                                                   : message_);
    }
  }

private:
  static void receive(LLVMDiagnosticInfoRef diagnostic, void *owner) noexcept {
    const auto severity = LLVMGetDiagInfoSeverity(diagnostic);
    if (severity != LLVMDSWarning && severity != LLVMDSError) { return; }
    auto &self = *static_cast<DiagnosticScope *>(owner);
    try {
      const Owner<char *, LLVMDisposeMessage> message(LLVMGetDiagInfoDescription(diagnostic));
      if (message) { self.message_ += message.get(); }
    } catch (...) { self.exception_ = std::current_exception(); }
  }

  LLVMContextRef context_;
  LLVMDiagnosticHandler handler_;
  void *owner_;
  std::string message_;
  std::exception_ptr exception_;
};

void verify(LLVMModuleRef module) {
  char *raw = nullptr;
  const auto failed = LLVMVerifyModule(module, LLVMReturnStatusAction, &raw);
  const Owner<char *, LLVMDisposeMessage> message(raw);
  if (failed != 0) { throw std::invalid_argument(message.get()); }
}

std::vector<std::byte> copy(LLVMMemoryBufferRef buffer) {
  const auto *first = reinterpret_cast<const std::byte *>(LLVMGetBufferStart(buffer));
  return {first, first + LLVMGetBufferSize(buffer)};
}

std::string name(LLVMValueRef value) {
  std::size_t size = 0;
  const auto *text = LLVMGetValueName2(value, &size);
  return {text, size};
}

std::string attribute(LLVMValueRef function, const std::string_view key) {
  auto *value =
      LLVMGetStringAttributeAtIndex(function,
                                    static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex),
                                    key.data(),
                                    static_cast<unsigned>(key.size()));
  if (value == nullptr) { return {}; }
  unsigned size = 0;
  const auto *text = LLVMGetStringAttributeValue(value, &size);
  return {text, size};
}

void features(LLVMModuleRef module, LLVMValueRef function) {
  const Owner<char *, LLVMDisposeMessage> cpu(LLVMGetHostCPUName());
  const auto available_features = native_features();
  std::string error;
  const std::string triple(LLVMGetTarget(module));
  const auto *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (target == nullptr) { throw std::runtime_error(error); }
  const std::unique_ptr<llvm::MCSubtargetInfo> native(
      target->createMCSubtargetInfo(triple, cpu.get(), available_features));
  if (!native) { throw std::runtime_error("Cannot resolve CPU12 native target features"); }
  const auto required_features = attribute(function, "target-features");
  std::string_view remaining(required_features);
  while (!remaining.empty()) {
    const auto end = remaining.find(',');
    const auto feature = remaining.substr(0, end);
    const auto known = native->getAllProcessorFeatures();
    const bool supported =
        feature.size() > 1 && std::ranges::any_of(known, [&](const auto &candidate) {
          return std::string_view(candidate.Key) == feature.substr(1);
        });
    if ((feature.starts_with('+') || feature.starts_with('-')) && !supported) {
      throw std::invalid_argument("CPU12 binding=" + name(function) +
                                  " requires unsupported target feature=" + std::string(feature));
    }
    if (end == std::string_view::npos) { break; }
    remaining.remove_prefix(end + 1);
  }
  auto required_cpu = attribute(function, "target-cpu");
  if (required_cpu.empty()) { required_cpu = cpu.get(); }
  if (!native->isCPUStringValid(required_cpu)) {
    throw std::invalid_argument("CPU12 binding=" + name(function) + " has unknown target CPU");
  }
  const std::unique_ptr<llvm::MCSubtargetInfo> required(
      target->createMCSubtargetInfo(triple, required_cpu, required_features));
  if (!required) {
    throw std::invalid_argument("CPU12 binding=" + name(function) + " has no target feature model");
  }
  const auto missing = required->getFeatureBits() & ~native->getFeatureBits();
  if (missing.any()) {
    std::string detail;
    for (const auto &feature : native->getAllProcessorFeatures()) {
      if (missing.test(feature.Value)) { detail += std::string(feature.Key) + ","; }
    }
    throw std::invalid_argument("CPU12 binding=" + name(function) +
                                " requires unsupported target features=" + detail);
  }
}

void compatible(LLVMModuleRef module, LLVMModuleRef incoming) {
  const std::string_view source(LLVMGetTarget(incoming));
  if (!source.empty()) {
    const llvm::Triple expected(LLVMGetTarget(module));
    const llvm::Triple actual(source);
    const bool same_os =
        expected.getOS() == actual.getOS() || (expected.isOSDarwin() && actual.isOSDarwin());
    if (expected.getArch() != actual.getArch() ||
        expected.getObjectFormat() != actual.getObjectFormat() || !same_os ||
        expected.getEnvironment() != actual.getEnvironment()) {
      throw std::invalid_argument("CPU12 binding target is incompatible with the native image");
    }
  }
  const std::string_view layout(LLVMGetDataLayoutStr(incoming));
  if (!layout.empty() && layout != LLVMGetDataLayoutStr(module)) {
    throw std::invalid_argument("CPU12 binding data layout is incompatible with the native image");
  }
  for (const auto *name : {"llvm.global_ctors", "llvm.global_dtors"}) {
    if (LLVMGetNamedGlobal(incoming, name) != nullptr) {
      throw std::invalid_argument(
          "CPU12 binding bitcode requires explicit initialization outside compilation");
    }
  }
  constexpr std::array names{"cpu12_read_byte",
                             "cpu12_read_word",
                             "cpu12_write_byte",
                             "cpu12_write_word",
                             "cpu12_read_masked_byte"};
  for (const auto *name : names) {
    auto *expected = LLVMGetNamedFunction(module, name);
    auto *actual = LLVMGetNamedFunction(incoming, name);
    if (expected != nullptr && actual != nullptr &&
        (LLVMGlobalGetValueType(expected) != LLVMGlobalGetValueType(actual) ||
         LLVMGetFunctionCallConv(expected) != LLVMGetFunctionCallConv(actual))) {
      throw std::invalid_argument("CPU12 binding ABI mismatch for " + std::string(name));
    }
  }
}

void address_constants(LLVMModuleRef module) {
  std::vector<LLVMValueRef> pending;
  for (auto *global = LLVMGetFirstGlobal(module); global != nullptr;
       global = LLVMGetNextGlobal(global)) {
    pending.push_back(global);
  }
  for (auto *function = LLVMGetFirstFunction(module); function != nullptr;
       function = LLVMGetNextFunction(function)) {
    for (auto *block = LLVMGetFirstBasicBlock(function); block != nullptr;
         block = LLVMGetNextBasicBlock(block)) {
      for (auto *instruction = LLVMGetFirstInstruction(block); instruction != nullptr;
           instruction = LLVMGetNextInstruction(instruction)) {
        pending.push_back(instruction);
      }
    }
  }
  std::set<LLVMValueRef> visited;
  while (!pending.empty()) {
    auto *value = pending.back();
    pending.pop_back();
    if (value == nullptr || !visited.insert(value).second || LLVMIsAUser(value) == nullptr) {
      continue;
    }
    const bool cast =
        (LLVMIsAConstantExpr(value) != nullptr && LLVMGetConstOpcode(value) == LLVMIntToPtr) ||
        (LLVMIsAInstruction(value) != nullptr && LLVMGetInstructionOpcode(value) == LLVMIntToPtr);
    if (cast) {
      auto *address = LLVMGetOperand(value, 0);
      if (LLVMIsConstant(address) != 0 && LLVMIsNull(address) == 0) {
        throw std::invalid_argument("CPU12 binding contains an absolute host pointer");
      }
    }
    for (int operand = 0; operand < LLVMGetNumOperands(value); ++operand) {
      pending.push_back(LLVMGetOperand(value, static_cast<unsigned>(operand)));
    }
  }
}
}

void link_bindings(LLVMModuleRef module, const std::span<const std::byte> bytes) {
  if (bytes.empty()) { return; }
  const Owner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> input(
      LLVMCreateMemoryBufferWithMemoryRangeCopy(
          reinterpret_cast<const char *>(bytes.data()), bytes.size(), "cpu12-bindings"));
  const DiagnosticScope diagnostics(LLVMGetModuleContext(module));
  LLVMModuleRef raw = nullptr;
  const auto failed = LLVMParseBitcodeInContext2(LLVMGetModuleContext(module), input.get(), &raw);
  Owner<LLVMModuleRef, LLVMDisposeModule> incoming(raw);
  diagnostics.check(failed != 0 || !incoming);
  verify(incoming.get());
  compatible(module, incoming.get());
  address_constants(incoming.get());
  LLVMSetDataLayout(incoming.get(), LLVMGetDataLayoutStr(module));
  LLVMSetTarget(incoming.get(), LLVMGetTarget(module));
  std::vector<std::string> functions;
  std::vector<std::string> globals;
  for (auto *function = LLVMGetFirstFunction(incoming.get()); function != nullptr;
       function = LLVMGetNextFunction(function)) {
    if (LLVMIsDeclaration(function) == 0) {
      features(module, function);
      functions.push_back(name(function));
    }
  }
  for (auto *global = LLVMGetFirstGlobal(incoming.get()); global != nullptr;
       global = LLVMGetNextGlobal(global)) {
    const auto symbol = name(global);
    if (symbol == "llvm.used" || symbol == "llvm.compiler.used") { continue; }
    if (LLVMIsDeclaration(global) == 0 && LLVMIsGlobalConstant(global) == 0) {
      throw std::invalid_argument("CPU12 binding state must be supplied through the runtime owner");
    }
    if (LLVMIsDeclaration(global) == 0) { globals.push_back(symbol); }
  }
  diagnostics.check(LLVMLinkModules2(module, incoming.release()) != 0);
  for (const auto &symbol : functions) {
    if (auto *function = LLVMGetNamedFunction(module, symbol.c_str())) {
      LLVMSetLinkage(function, LLVMPrivateLinkage);
    }
  }
  for (const auto &symbol : globals) {
    if (auto *global = LLVMGetNamedGlobal(module, symbol.c_str())) {
      LLVMSetLinkage(global, LLVMPrivateLinkage);
    }
  }
  verify(module);
}

void native_attributes(LLVMValueRef function) {
  const Owner<char *, LLVMDisposeMessage> cpu(LLVMGetHostCPUName());
  const auto features = native_features();
  LLVMAddTargetDependentFunctionAttr(function, "target-cpu", cpu.get());
  LLVMAddTargetDependentFunctionAttr(function, "target-features", features.c_str());
}

std::string native_features() {
  const Owner<char *, LLVMDisposeMessage> triple(LLVMGetDefaultTargetTriple());
  const Owner<char *, LLVMDisposeMessage> cpu(LLVMGetHostCPUName());
  const Owner<char *, LLVMDisposeMessage> flags(LLVMGetHostCPUFeatures());
  std::string result;
  if (llvm::Triple(triple.get()).isAArch64()) {
    const auto model = llvm::AArch64::parseCpu(cpu.get());
    if (!model) { throw std::runtime_error("Cannot resolve native AArch64 CPU extensions"); }
    llvm::AArch64::ExtensionSet extensions;
    extensions.addArchDefaults(model->Arch);
    extensions.addCPUDefaults(*model);
    std::vector<std::string> features;
    extensions.toLLVMFeatureList(features);
    for (const auto &feature : features) { result += feature + ","; }
  }
  result += flags.get();
  if (result.ends_with(',')) { result.pop_back(); }
  return result;
}

std::vector<std::byte> bitcode(LLVMModuleRef module) {
  verify(module);
  const Owner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> output(
      LLVMWriteBitcodeToMemoryBuffer(module));
  if (!output) { throw std::runtime_error("Cannot export CPU12 semantic bitcode"); }
  return copy(output.get());
}

std::vector<std::byte> object(LLVMModuleRef module, LLVMTargetMachineRef machine) {
  char *raw = nullptr;
  LLVMMemoryBufferRef buffer = nullptr;
  const auto failed =
      LLVMTargetMachineEmitToMemoryBuffer(machine, module, LLVMObjectFile, &raw, &buffer);
  const Owner<char *, LLVMDisposeMessage> message(raw);
  const Owner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> output(buffer);
  if (failed != 0 || !output) {
    throw std::runtime_error(message ? message.get() : "Cannot export CPU12 native object");
  }
  return copy(output.get());
}
}
