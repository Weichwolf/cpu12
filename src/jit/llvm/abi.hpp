// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "llvm-c/Types.h"

#include <string>

namespace cpu12::detail::llvm19 {
[[nodiscard]] std::string function_abi(LLVMValueRef value);
}
