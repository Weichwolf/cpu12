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

// The caller declares a known, writable RAM window whose storage outlives Core.
struct Ram {
  std::uint16_t first = 0;
  std::span<std::uint8_t> bytes;
};

// Callbacks perform real external accesses. False means an explicit fault.
// They must not throw; state/cache mutation and recursive execution are refused.
struct ByteWrite {
  std::uint16_t address = 0;
  std::uint8_t value = 0;
};

struct WordWrite {
  std::uint16_t address = 0, value = 0;
};

struct Io {
  void *owner = nullptr;
  bool (*read)(void *, std::uint16_t, std::uint8_t &) noexcept = nullptr;
  bool (*write)(void *, ByteWrite) noexcept = nullptr;
  bool (*read_word)(void *, std::uint16_t, std::uint16_t &) noexcept = nullptr;
  bool (*write_word)(void *, WordWrite) noexcept = nullptr;
};
enum class Fault : std::uint8_t {
  none,
  invalid_input,
  unsupported_instruction,
  unsupported_host,
  memory_read,
  memory_write,
  reentrant
};

struct RunResult {
  std::uint32_t retired = 0;
  Fault fault = Fault::none;
  std::uint16_t pc = 0;
  std::uint8_t opcode = 0;
  std::uint16_t address = 0;
  std::uint16_t value = 0;
  std::uint8_t access_size = 0;
};

struct Statistics {
  std::uint64_t native_instructions = 0, compiled_entries = 0, cache_resets = 0;
  std::uint64_t read_callbacks = 0, write_callbacks = 0;
};

// Captured instructions have already been fetched by the caller. Board timing,
// code fetch and interrupts are not implemented by this initial API. Bus faults
// latch until explicit register replacement, preventing automatic access replay.
class Core {
public:
  explicit Core(Ram ram = {}, Io io = {});
  ~Core();
  Core(const Core &) = delete;
  Core &operator=(const Core &) = delete;
  [[nodiscard]] static bool native_supported() noexcept;
  [[nodiscard]] Registers registers() const noexcept;
  // An explicit fresh boundary clears a latched bus fault; coordinate bus state externally.
  void set_registers(Registers registers);
  [[nodiscard]] RunResult execute(std::span<const std::uint8_t> bytes);
  [[nodiscard]] Statistics statistics() const noexcept;
  void clear_code_cache();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
