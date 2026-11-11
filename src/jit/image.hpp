// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/block.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace cpu12::detail {
using cpu12::CodeImage;
using cpu12::CodeSpan;
using cpu12::CodeVariant;

class ImagePlan {
public:
  explicit ImagePlan(CodeImage image);

  struct Variant {
    struct Source {
      Address address;
      std::vector<Byte> words;
    };

    std::uint64_t id;
    std::vector<Source> code;
    std::vector<BlockInstruction> instructions;
    std::vector<Address> entries;
  };

  [[nodiscard]] std::span<const Variant> variants() const noexcept { return variants_; }

private:
  std::vector<Variant> variants_;
};
}
