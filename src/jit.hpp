// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "runtime.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace cpu12::detail {
class Jit {
public:
  explicit Jit(Ram ram, Options options);
  ~Jit();
  Jit(const Jit &) = delete;
  Jit &operator=(const Jit &) = delete;
  // False means unsupported captured bytes, with no native or I/O effect.
  [[nodiscard]] bool
  execute(std::span<const std::uint8_t> bytes, Registers &registers, NativeContext &context);
  void interrupt(InterruptRequest request, Registers &registers, NativeContext &context);
  void resume_stop(Registers &registers, NativeContext &context);
  void clear();
  [[nodiscard]] Statistics statistics() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
