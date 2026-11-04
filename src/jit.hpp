// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "runtime.hpp"

#include <memory>

namespace cpu12::detail {
class Jit {
public:
  explicit Jit(Ram ram, Options options);
  ~Jit();
  Jit(const Jit &) = delete;
  Jit &operator=(const Jit &) = delete;
  void execute(const Instruction &instruction, Registers &registers, NativeContext &context);
  void clear();
  [[nodiscard]] Statistics statistics() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
