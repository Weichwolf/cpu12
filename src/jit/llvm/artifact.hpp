// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "jit/llvm/callback_binding.hpp"

#include "llvm-c/TargetMachine.h"
#include "llvm-c/Types.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace cpu12::detail::llvm19 {
void link_bindings(LLVMModuleRef module,
                   std::span<const std::byte> bytes,
                   std::span<const CallbackBinding> callbacks = {});
void native_attributes(LLVMValueRef function);
void optimize_module(LLVMModuleRef module, LLVMTargetMachineRef machine);
[[nodiscard]] std::string native_features();
[[nodiscard]] std::vector<std::byte> bitcode(LLVMModuleRef module);
[[nodiscard]] std::vector<std::byte> object(LLVMModuleRef module, LLVMTargetMachineRef machine);
}
