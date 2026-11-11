// SPDX-License-Identifier: GPL-3.0-only
#include "semantic_consumer.hpp"

#include "llvm-c/Error.h"
#include "llvm-c/LLJIT.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Types.h"

#include <atomic>
#include <bit>
#include <dlfcn.h>
#include <stdexcept>

namespace {
std::atomic<unsigned> machines{0}, engines{0}, objects{0}, starts{0}, stops{0};

template <typename Function> Function original(const char *name) {
  const auto *address = dlsym(RTLD_NEXT, name);
  if (address == nullptr) { throw std::runtime_error("Cannot resolve the packaged LLVM API"); }
  return std::bit_cast<Function>(address);
}
}

extern "C" LLVMTargetMachineRef LLVMCreateTargetMachine(LLVMTargetRef target,
                                                        const char *triple,
                                                        const char *cpu,
                                                        const char *features,
                                                        LLVMCodeGenOptLevel level,
                                                        LLVMRelocMode relocation,
                                                        LLVMCodeModel model) {
  ++machines;
  return original<decltype(&LLVMCreateTargetMachine)>("LLVMCreateTargetMachine")(
      target, triple, cpu, features, level, relocation, model);
}

extern "C" LLVMErrorRef LLVMOrcCreateLLJIT(LLVMOrcLLJITRef *result,
                                           LLVMOrcLLJITBuilderRef builder) {
  ++engines;
  return original<decltype(&LLVMOrcCreateLLJIT)>("LLVMOrcCreateLLJIT")(result, builder);
}

extern "C" LLVMBool LLVMTargetMachineEmitToMemoryBuffer(LLVMTargetMachineRef T,
                                                        LLVMModuleRef M,
                                                        LLVMCodeGenFileType codegen,
                                                        char **ErrorMessage,
                                                        LLVMMemoryBufferRef *OutMemBuf) {
  ++objects;
  return original<decltype(&LLVMTargetMachineEmitToMemoryBuffer)>(
      "LLVMTargetMachineEmitToMemoryBuffer")(T, M, codegen, ErrorMessage, OutMemBuf);
}

namespace cpu12::test::semantic {
Calls calls() noexcept {
  return {.machines = machines.load(),
          .engines = engines.load(),
          .objects = objects.load(),
          .initialized = starts.load(),
          .finalized = stops.load()};
}

void initialized() noexcept {
  ++starts;
}

void finalized() noexcept {
  ++stops;
}
}
