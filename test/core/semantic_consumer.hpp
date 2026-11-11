// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "cpu12/core.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace cpu12::test::semantic {
struct Calls {
  unsigned machines = 0, engines = 0, objects = 0, initialized = 0, finalized = 0;
  bool operator==(const Calls &) const = default;
};

[[nodiscard]] Calls calls() noexcept;
void initialized() noexcept;
void finalized() noexcept;

class Code {
public:
  explicit Code(std::span<const SemanticImage> images, std::span<const std::byte> host);
  ~Code();
  Code(const Code &) = delete;
  Code &operator=(const Code &) = delete;
  [[nodiscard]] std::vector<NativeSymbol> symbols(const SemanticImage &image) const;
  [[nodiscard]] std::uint64_t bytes() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
