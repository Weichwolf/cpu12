// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Types.h"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace cpu12::detail::llvm19 {
void link_bindings(LLVMModuleRef module, std::span<const std::byte> bytes);
void native_attributes(LLVMValueRef function);
[[nodiscard]] std::string native_features();
[[nodiscard]] std::vector<std::byte> bitcode(LLVMModuleRef module);
[[nodiscard]] std::vector<std::byte> object(LLVMModuleRef module, LLVMTargetMachineRef machine);
}
