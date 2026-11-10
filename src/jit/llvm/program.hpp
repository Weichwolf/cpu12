// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "jit/block.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace cpu12::detail::llvm19 {
class Program {
public:
  explicit Program(std::span<const BlockInstruction> instructions);
  ~Program();
  Program(const Program &) = delete;
  Program &operator=(const Program &) = delete;
  [[nodiscard]] const void *entry() const noexcept;
  [[nodiscard]] std::uint64_t native_bytes() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
