// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/image.hpp"
#include "jit/llvm/callback_binding.hpp"
#include "jit/llvm/program.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>

namespace cpu12::detail::llvm19 {
using cpu12::RegionLayout;
using cpu12::VariantLayout;

class ImageProgram {
public:
  ImageProgram(CodeImage image,
               std::span<const VariantLayout> layouts,
               std::span<const std::byte> bindings = {},
               std::span<const InterruptRequest> interrupts = {},
               std::span<const CodeTransition> transitions = {},
               std::span<const CallbackBinding> callbacks = {},
               ArtifactOutput output = ArtifactOutput::native,
               const std::string &prefix = "cpu12_");
  ImageProgram(const ImageProgram &semantic, const NativeSymbols &symbols);
  ~ImageProgram();
  ImageProgram(const ImageProgram &) = delete;
  ImageProgram &operator=(const ImageProgram &) = delete;
  [[nodiscard]] const Program &program(std::uint64_t id) const;
  [[nodiscard]] const void *entry(std::uint64_t id, Address pc) const;
  [[nodiscard]] std::span<const BlockInstruction> instructions(std::uint64_t id) const;
  [[nodiscard]] PreparationReport report() const noexcept;
  [[nodiscard]] std::span<const ImagePlan::Variant> variants() const noexcept;
  [[nodiscard]] std::span<const RegionLayout> layout(std::uint64_t id) const;
  [[nodiscard]] std::uint64_t foreground_prefix(std::uint64_t id, Address pc) const;
  [[nodiscard]] std::uint64_t
  interrupt_entry_prefix(std::uint64_t id, const State &state, const Memory &memory) const;
  [[nodiscard]] const CodeTargets &transitions(std::uint64_t id) const;
  void validate_binding(std::uint64_t id, const Memory &memory) const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
