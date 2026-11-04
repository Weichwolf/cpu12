// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "instruction.hpp"

#include <memory>

namespace cpu12::detail {
class Jit {
public:
  Jit();
  ~Jit();
  Jit(const Jit &) = delete;
  Jit &operator=(const Jit &) = delete;
  void execute(const Instruction &instruction, Registers &registers);
  void clear();
  [[nodiscard]] Statistics statistics() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
