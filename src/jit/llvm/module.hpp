// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "cpu12/core.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace cpu12::detail::llvm19 {
class NativeModule {
public:
  NativeModule(std::span<const std::byte> bitcode,
               std::span<const SemanticExport> exports,
               std::span<const std::byte> bindings,
               std::span<const CallbackBinding> callbacks);
  ~NativeModule();
  NativeModule(const NativeModule &) = delete;
  NativeModule &operator=(const NativeModule &) = delete;
  [[nodiscard]] std::span<const NativeSymbol> symbols() const noexcept;
  [[nodiscard]] std::uint64_t native_bytes() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
