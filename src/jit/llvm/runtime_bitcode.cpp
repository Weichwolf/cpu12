// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/runtime_bitcode.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace cpu12::detail::llvm19 {
std::span<const std::byte> runtime_bitcode() noexcept {
  static constexpr auto bytes = std::to_array(
#include "runtime_bitcode.inc"
  );
  return std::as_bytes(std::span(bytes).first(bytes.size() - 1));
}
}
