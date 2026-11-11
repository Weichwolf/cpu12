// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "jit/llvm/callback_binding.hpp"

#include "llvm-c/Types.h"

#include <cstdint>
#include <span>

namespace cpu12::detail::llvm19 {
enum class CallbackStage : std::uint8_t { definitions, declarations };
void link_callbacks(LLVMModuleRef module,
                    std::span<const CallbackBinding> bindings,
                    CallbackStage stage = CallbackStage::definitions);
void validate_callbacks(LLVMModuleRef module,
                        LLVMModuleRef incoming,
                        std::span<const CallbackBinding> bindings);
void require_callbacks(LLVMModuleRef module, std::span<const CallbackBinding> bindings);
}
