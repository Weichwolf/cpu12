// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>

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
  instruction_fetch,
  unprepared_execution
};

enum class ExecutionState : std::uint8_t { running, waiting, stopped };

struct RunResult {
  std::uint32_t retired = 0;
  Fault fault = Fault::none;
  ExecutionState state = ExecutionState::running;
  std::uint16_t pc = 0;
  std::uint8_t opcode = 0;
  std::uint16_t address = 0;
  std::uint16_t value = 0;
  std::uint8_t access_size = 0;
  std::uint8_t read_mask = 0;
  bool operator==(const RunResult &) const = default;
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

// Read-only inspection of Core-owned CPU state; memory and I/O belong to the consumer.
struct State {
  Registers registers;
  RunResult failure;
  ExecutionState execution = ExecutionState::running;
  std::uint8_t irq_delay = 0;
  bool returned_frame = false;
  bool operator==(const State &) const = default;
};

struct Progress {
  // Completed nominal internal-bus cycles, including accepted reset/IRQ entries.
  // Monotonic for the Core lifetime; external waits belong to the owner.
  std::uint64_t estimated_cycles = 0;
  bool operator==(const Progress &) const = default;
};

struct Statistics {
  // Translation-only accounting; execution and transport observers belong to the owner.
  std::uint64_t compiled_entries = 0;
};

struct PreparationReport {
  std::uint64_t prepared_entries = 0, unresolved_entries = 0, native_bytes = 0;
  RunResult failure;
};

struct CodeSpan {
  Address address;
  std::span<const Byte> words;
};

struct CodeVariant {
  std::uint64_t id;
  std::span<const CodeSpan> code;
  std::span<const Address> entries;
};

struct CodeImage {
  std::span<const CodeVariant> variants;
};

struct RegionLayout {
  Address first;
  std::size_t size;
  bool writable = true, tracked = false;
};

struct VariantLayout {
  std::uint64_t id;
  std::span<const RegionLayout> memory;
};

struct CodeTransition {
  std::uint64_t from, to;
  Address entry;
};

enum class CallbackRole : std::uint8_t { read, write, read_word, write_word, read_masked };

struct CallbackBinding {
  CallbackRole role;
  std::string_view symbol;
};

struct CompileOptions {
  std::span<const VariantLayout> memory;
  std::span<const InterruptRequest> interrupts;
  // An absent resident target may yield only to these explicitly prepared versions.
  std::span<const CodeTransition> transitions;
  std::span<const CallbackBinding> callbacks;
  std::span<const std::byte> bindings;
};

struct SemanticExport {
  std::string_view name, abi;
};

struct NativeSymbol {
  std::string_view name;
  const void *address;
};

struct NativeSymbols {
  std::shared_ptr<const void> owner;
  std::span<const NativeSymbol> symbols;
  std::string_view identity;
  std::uint64_t native_bytes = 0;
};

class SemanticImage {
public:
  SemanticImage() = default;
  [[nodiscard]] std::span<const std::byte> bitcode() const noexcept;
  [[nodiscard]] std::span<const SemanticExport> exports() const noexcept;
  [[nodiscard]] std::string_view identity() const noexcept;

private:
  class Impl;
  explicit SemanticImage(std::shared_ptr<const Impl> image) noexcept;
  std::shared_ptr<const Impl> impl_;
  friend class Core;
  friend class NativeImage;
};

class NativeImage {
public:
  NativeImage() = default;
  [[nodiscard]] PreparationReport report() const noexcept;

private:
  class Impl;
  explicit NativeImage(std::shared_ptr<const Impl> image) noexcept;
  std::shared_ptr<const Impl> impl_;
  friend class Core;
};

// The board owns clocks, pending-source arbitration and acknowledgement. Native
// faults latch until explicit register replacement or a recovered reset boundary.
class Core {
public:
  explicit Core(Memory memory = Memory::external(), Io *io = nullptr);
  ~Core();
  Core(const Core &) = delete;
  Core &operator=(const Core &) = delete;
  [[nodiscard]] static SemanticImage lower(CodeImage image, CompileOptions options);
  [[nodiscard]] static NativeImage bind(const SemanticImage &image, const NativeSymbols &symbols);
  [[nodiscard]] static NativeImage compile(CodeImage image, CompileOptions options);
  // Cold owner boundary: validate code/layout and bind RAM/events before publishing.
  void install(const NativeImage &image, std::uint64_t variant);
  [[nodiscard]] Registers registers() const noexcept;
  // A live read-only view, valid for this Core lifetime; inspect on the execution owner.
  [[nodiscard]] const State &state() const noexcept;
  [[nodiscard]] Progress progress() const noexcept;
  // Clears a latched fault. Pending code transfers require installation or reset.
  void set_registers(Registers registers);
  // An installed image requires a matching install after every explicit remap.
  void set_memory(Memory memory);
  [[nodiscard]] PreparationReport prepare(std::span<const Address> established_entries);
  [[nodiscard]] RunResult run_instruction(std::span<const std::uint8_t> bytes);
  [[nodiscard]] RunResult step();
  [[nodiscard]] EventResult reset(std::uint16_t vector) noexcept;
  [[nodiscard]] bool interrupt_ready(InterruptKind kind) const noexcept;
  [[nodiscard]] bool interrupt_frame_available() const noexcept;
  // The board has completed RTI's pending-source poll before later refill clocks.
  void close_return_window();
  [[nodiscard]] EventResult interrupt(InterruptRequest request);
  // Report an asserted masked XIRQ while stopped; no vector is serviced.
  [[nodiscard]] EventResult resume_from_stop();
  // Runs until a fault, sleep, prepared transfer or instruction-counter limit.
  // Explicit limits give deterministic control; the default needs no consumer quantum.
  [[nodiscard]] RunResult
  run(std::uint32_t instruction_budget = std::numeric_limits<std::uint32_t>::max());
  // Nominal internal-bus deadline; external waits and device time belong to the
  // owner. The final atomic instruction can finish beyond the requested boundary.
  [[nodiscard]] RunResult run_until(std::uint64_t base_cycle_deadline);
  // Owner-thread I/O may request return after the current complete instruction.
  void request_yield() noexcept;
  [[nodiscard]] Statistics statistics() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
