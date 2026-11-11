// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "jit/llvm/callback_binding.hpp"

#include "llvm-c/Types.h"

#include <span>

namespace cpu12::detail::llvm19 {
void link_callbacks(LLVMModuleRef module, std::span<const CallbackBinding> bindings);
}
