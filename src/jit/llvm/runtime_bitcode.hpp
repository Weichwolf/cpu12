// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <span>

namespace cpu12::detail::llvm19 {
[[nodiscard]] std::span<const std::byte> runtime_bitcode() noexcept;
}
