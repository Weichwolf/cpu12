// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <memory>
#include <span>

namespace cpu12 {
struct Registers {
  std::uint16_t pc = 0, d = 0, x = 0, y = 0, sp = 0;
  std::uint8_t ccr = 0;
  bool operator==(const Registers &) const = default;
};
enum class Fault : std::uint8_t { none, invalid_input, unsupported_instruction, unsupported_host };

struct RunResult {
  std::uint32_t retired = 0;
  Fault fault = Fault::none;
  std::uint16_t pc = 0;
  std::uint8_t opcode = 0;
};

struct Statistics {
  std::uint64_t native_instructions = 0, compiled_entries = 0, cache_resets = 0;
};

// Captured instructions have already been fetched by the caller. Board timing,
// code fetch, interrupts and peripherals are not implemented by this initial API.
class Core {
public:
  Core();
  ~Core();
  Core(const Core &) = delete;
  Core &operator=(const Core &) = delete;
  [[nodiscard]] static bool native_supported() noexcept;
  [[nodiscard]] Registers registers() const noexcept;
  void set_registers(Registers registers) noexcept;
  [[nodiscard]] RunResult execute(std::span<const std::uint8_t> bytes);
  [[nodiscard]] Statistics statistics() const noexcept;
  void clear_code_cache();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
