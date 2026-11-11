// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include "llvm-c/Types.h"

#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace cpu12::detail::llvm19 {
[[nodiscard]] std::map<std::uint64_t, std::string>
lift_events(LLVMModuleRef module,
            const Memory &memory,
            const std::string &prefix,
            std::span<const InterruptRequest> interrupts,
            bool native);
}
