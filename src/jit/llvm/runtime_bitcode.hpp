// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace cpu12::detail::llvm19 {
struct TargetDescription {
  std::string triple, layout;
};

[[nodiscard]] std::span<const std::byte> runtime_bitcode() noexcept;
[[nodiscard]] TargetDescription runtime_target();
}
