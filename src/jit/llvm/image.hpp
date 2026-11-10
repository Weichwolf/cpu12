// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "jit/image.hpp"
#include "jit/llvm/program.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace cpu12::detail::llvm19 {
struct RegionLayout {
  Address first;
  std::size_t size;
  bool writable = true, tracked = false;
};

struct VariantLayout {
  std::uint64_t id;
  std::span<const RegionLayout> memory;
};

class ImageProgram {
public:
  ImageProgram(CodeImage image,
               std::span<const VariantLayout> layouts,
               std::span<const std::byte> bindings = {});
  ~ImageProgram();
  ImageProgram(const ImageProgram &) = delete;
  ImageProgram &operator=(const ImageProgram &) = delete;
  [[nodiscard]] const Program &program(std::uint64_t id) const;
  [[nodiscard]] const void *entry(std::uint64_t id, Address pc) const;
  [[nodiscard]] std::span<const BlockInstruction> instructions(std::uint64_t id) const;
  void validate_binding(std::uint64_t id, const Memory &memory) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
