// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "jit/block.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace cpu12::detail::llvm19 {
class Program {
public:
  Program(std::span<const BlockInstruction> instructions, const Memory &memory);
  ~Program();
  Program(const Program &) = delete;
  Program &operator=(const Program &) = delete;
  [[nodiscard]] const void *entry(Address address) const;
  // Generated functions contain no captured RAM pointers. Reuse requires the
  // complete immutable region's exact bytes and any lowered memory topology.
  [[nodiscard]] const void *find(std::span<const BlockInstruction> region,
                                 const Memory &memory) const;
  [[nodiscard]] std::uint64_t native_bytes() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
