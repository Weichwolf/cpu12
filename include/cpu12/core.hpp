// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>

namespace cpu12 {
inline constexpr std::size_t maximum_capture_bytes = 6;

struct Registers {
  std::uint16_t pc = 0, d = 0, x = 0, y = 0, sp = 0;
  std::uint8_t ccr = 0;
  bool operator==(const Registers &) const = default;
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

struct MaskedByteRead {
  std::uint16_t address = 0;
  std::uint8_t mask = std::numeric_limits<std::uint8_t>::max();
};

struct Io {
  void *owner = nullptr;
  bool (*read)(void *, std::uint16_t, std::uint8_t &) noexcept = nullptr;
  bool (*write)(void *, ByteWrite) noexcept = nullptr;
  bool (*read_word)(void *, std::uint16_t, std::uint16_t &) noexcept = nullptr;
  bool (*write_word)(void *, WordWrite) noexcept = nullptr;
  bool (*fetch)(void *, std::uint16_t, std::uint8_t &) noexcept = nullptr;
  bool (*read_masked)(void *, MaskedByteRead, std::uint8_t &) noexcept = nullptr;
};
enum class Fault : std::uint8_t {
  none,
  invalid_input,
  unsupported_instruction,
  unsupported_host,
  memory_read,
  memory_write,
  reentrant,
  unsupported_state,
  instruction_fetch
};

struct RunResult {
  std::uint32_t retired = 0;
  Fault fault = Fault::none;
  std::uint16_t pc = 0;
  std::uint8_t opcode = 0;
  std::uint16_t address = 0;
  std::uint16_t value = 0;
  std::uint8_t access_size = 0;
  std::uint8_t read_mask = 0;
};

enum class InterruptKind : std::uint8_t { maskable, xirq };

struct InterruptRequest {
  std::uint16_t vector = 0;
  InterruptKind kind = InterruptKind::maskable;
};

struct EventResult {
  bool accepted = false;
  Fault fault = Fault::none;
  std::uint16_t pc = 0, address = 0, value = 0;
  std::uint8_t access_size = 0;
};

struct Options {
  // Bounded native code storage; each compiled entry occupies one host page.
  std::size_t cache_entries = 1024;
};

struct Statistics {
  std::uint64_t native_instructions = 0, compiled_entries = 0, cache_resets = 0;
  std::uint64_t read_callbacks = 0, write_callbacks = 0;
  std::uint64_t fetch_callbacks = 0;
};

// The board owns clocks, pending-source arbitration and acknowledgement. Native
// faults latch until explicit register replacement or a recovered reset boundary.
class Core {
public:
  explicit Core(Memory memory = Memory::external(), Io *io = nullptr, Options options = {});
  // Raw-window compatibility entry; new integrations use Memory explicitly.
  explicit Core(Ram ram, Io io = {});
  ~Core();
  Core(const Core &) = delete;
  Core &operator=(const Core &) = delete;
  [[nodiscard]] static bool native_supported() noexcept;
  [[nodiscard]] Registers registers() const noexcept;
  // An explicit fresh boundary clears a latched fault; coordinate bus state externally.
  void set_registers(Registers registers);
  [[nodiscard]] RunResult run_instruction(std::span<const std::uint8_t> bytes);
  [[nodiscard]] RunResult step();
  [[nodiscard]] EventResult reset(std::uint16_t vector) noexcept;
  [[nodiscard]] bool interrupt_ready(InterruptKind kind) const noexcept;
  [[nodiscard]] EventResult interrupt(InterruptRequest request);
  // Instruction budget, not a cycle budget. The result counts completed instructions.
  [[nodiscard]] RunResult run(std::uint32_t instruction_budget);
  [[nodiscard]] Statistics statistics() const noexcept;
  void clear_code_cache();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
