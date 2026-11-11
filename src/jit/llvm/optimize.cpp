// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/lifetime.hpp"

#include "llvm-c/Analysis.h"
#include "llvm-c/Core.h"
#include "llvm-c/Error.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Transforms/PassBuilder.h"
#include "llvm-c/Types.h"

#include <stdexcept>
#include <string_view>

namespace cpu12::detail::llvm19 {
namespace {
void checked(LLVMErrorRef error) {
  if (error == nullptr) { return; }
  const Owner<char *, LLVMDisposeErrorMessage> message(LLVMGetErrorMessage(error));
  throw std::runtime_error(message.get());
}

void verify(LLVMModuleRef module) {
  char *raw = nullptr;
  const auto failed = LLVMVerifyModule(module, LLVMReturnStatusAction, &raw);
  const Owner<char *, LLVMDisposeMessage> message(raw);
  if (failed != 0) { throw std::runtime_error(message.get()); }
}
}

void optimize_module(LLVMModuleRef module, LLVMTargetMachineRef machine) {
  constexpr std::string_view interface = "cpu12-core-interface";
  for (auto *function = LLVMGetFirstFunction(module); function != nullptr;
       function = LLVMGetNextFunction(function)) {
    if (LLVMGetStringAttributeAtIndex(function,
                                      static_cast<LLVMAttributeIndex>(LLVMAttributeFunctionIndex),
                                      interface.data(),
                                      interface.size()) != nullptr) {
      LLVMSetLinkage(function, LLVMInternalLinkage);
    }
  }
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
  checked(LLVMRunPasses(module, "default<O3>", machine, options.get()));
#if __has_feature(address_sanitizer)
  checked(LLVMRunPasses(module, "asan", machine, options.get()));
#endif
  verify(module);
}
}
