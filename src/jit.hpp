// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/block.hpp"
#include "runtime.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace cpu12::detail {
namespace llvm19 {
class ImageProgram;
}

class Jit {
public:
  explicit Jit(const Memory &ram);
  ~Jit();
  void set_memory(const Memory &memory);
  void install(std::shared_ptr<const llvm19::ImageProgram> image,
               std::span<const std::uint64_t> variants,
               std::span<const InterruptRequest> interrupts);
  void isolate_reads(std::span<const LocalRead> reads);
  [[nodiscard]] bool requires_image() const noexcept;
  [[nodiscard]] bool transition_pending() const noexcept;
  [[nodiscard]] std::uint64_t foreground_prefix(const State &state, bool local = false) const;
  [[nodiscard]] std::uint64_t event_prefix(const State &state, bool local = false) const;
  void cancel_transition() noexcept;
  [[nodiscard]] Fault validate_instruction(Address pc, std::span<const Byte> bytes) const;
  [[nodiscard]] PreparationReport prepare(std::span<const Address> established_entries);
  [[nodiscard]] const Memory &memory() const noexcept;
  [[nodiscard]] const MemoryBinding *bindings() const noexcept;
  Jit(const Jit &) = delete;
  Jit &operator=(const Jit &) = delete;
  // False means unsupported captured bytes, with no native or I/O effect.
  [[nodiscard]] bool
  execute(std::span<const std::uint8_t> bytes, Registers &registers, NativeContext &context);
  [[nodiscard]] std::optional<NativeResult>
  run_block(std::uint32_t budget, Registers &registers, NativeContext &context);
  [[nodiscard]] NativeResult
  run_prepared(std::uint32_t budget, Registers &registers, NativeContext &context);
  void interrupt(InterruptRequest request, Registers &registers, NativeContext &context);
  void resume_stop(Registers &registers, NativeContext &context);
  [[nodiscard]] Statistics statistics() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
