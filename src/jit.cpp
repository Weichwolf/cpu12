// SPDX-License-Identifier: GPL-3.0-only
// The typed SysV bridge and W^X allocation pattern are adapted from the independent
// Weichwolf/dsp56300 project (GPL-3.0-only). No foreign emulator is used.
#include "jit.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/arena.hpp"
#include "jit/emitter.hpp"
#include "jit/interrupt.hpp"
#include "runtime.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>

namespace cpu12::detail {
namespace {
#if defined(__x86_64__) && defined(__linux__)
extern "C" void cpu12_enter_generated(const void *, Registers *, NativeContext *) noexcept;
asm(".pushsection .text\n"
    ".globl cpu12_enter_generated\n"
    ".hidden cpu12_enter_generated\n"
    ".type cpu12_enter_generated,@function\n"
    "cpu12_enter_generated:\n"
    "endbr64\n"
    "mov %rdi,%rax\n"
    "mov %rsi,%rdi\n"
    "mov %rdx,%rsi\n"
    "jmp *%rax\n"
    ".size cpu12_enter_generated,.-cpu12_enter_generated\n"
    ".popsection\n");
#endif
void enter(const void *entry, Registers &registers, NativeContext &context) noexcept {
#if defined(__x86_64__) && defined(__linux__)
  cpu12_enter_generated(entry, &registers, &context);
#else
  static_cast<void>(entry);
  static_cast<void>(registers);
  static_cast<void>(context);
#endif
}

}

class Jit::Impl {
public:
  Ram ram;
  ExecutableArena arena;
  std::unordered_map<std::uint64_t, const void *> blocks;
  Statistics statistics;
};

Jit::Jit(const Ram ram) : impl_(std::make_unique<Impl>()) {
  impl_->ram = ram;
}

Jit::~Jit() = default;

bool Jit::execute(const std::span<const std::uint8_t> bytes,
                  Registers &registers,
                  NativeContext &context) {
  constexpr unsigned length_shift = 48;
  constexpr unsigned opcode_shift = 40;
  constexpr unsigned postbyte_shift = 32;
  constexpr unsigned high_shift = 24;
  constexpr unsigned low_shift = 16;
  constexpr unsigned byte_shift = 8;
  static_assert(maximum_capture_bytes * byte_shift <= length_shift);
  // Include every raw byte and the exact length. Trailing masks/displacements
  // and malformed padded captures must never hit another native entry.
  auto key = (static_cast<std::uint64_t>(bytes.size()) << length_shift) |
             (static_cast<std::uint64_t>(bytes[0]) << opcode_shift);
  if (bytes.size() > 1) { key |= static_cast<std::uint64_t>(bytes[1]) << postbyte_shift; }
  if (bytes.size() > 2) { key |= static_cast<std::uint64_t>(bytes[2]) << high_shift; }
  if (bytes.size() > 3) { key |= static_cast<std::uint64_t>(bytes[3]) << low_shift; }
  if (bytes.size() > 4) { key |= static_cast<std::uint64_t>(bytes[4]) << byte_shift; }
  if (bytes.size() > 5) { key |= bytes[5]; }
  auto found = impl_->blocks.find(key);
  if (found == impl_->blocks.end()) {
    const auto instruction = decode(bytes);
    if (!instruction) { return false; }
    found = impl_->blocks.emplace(key, impl_->arena.append(emit(*instruction, impl_->ram))).first;
    ++impl_->statistics.compiled_entries;
  }
  enter(found->second, registers, context);
  if (context.fault == Fault::none) {
    ++impl_->statistics.native_instructions;
    if (context.cpu.irq_delay != 0) { --context.cpu.irq_delay; }
    if (context.cpu.returned_frame) {
      constexpr unsigned rti = 0x0b;
      constexpr auto rti_key =
          (std::uint64_t{1} << length_shift) | (std::uint64_t{rti} << opcode_shift);
      if (key != rti_key) { context.cpu.returned_frame = false; }
    }
  }
  return true;
}

void Jit::interrupt(const InterruptRequest request, Registers &registers, NativeContext &context) {
  constexpr unsigned kind_shift = 16;
  constexpr auto event_tag = std::uint64_t{1} << 63U;
  constexpr unsigned frame_shift = 17;
  auto frame = InterruptFrame::push;
  if (context.cpu.execution != ExecutionState::running) {
    frame = InterruptFrame::waiting;
  } else if (context.cpu.returned_frame) {
    frame = InterruptFrame::returned;
  }
  const auto key = event_tag | (static_cast<std::uint64_t>(frame) << frame_shift) |
                   (static_cast<std::uint64_t>(request.kind) << kind_shift) | request.vector;
  auto found = impl_->blocks.find(key);
  if (found == impl_->blocks.end()) {
    found =
        impl_->blocks.emplace(key, impl_->arena.append(emit_interrupt(request, impl_->ram, frame)))
            .first;
    ++impl_->statistics.compiled_entries;
  }
  enter(found->second, registers, context);
}

void Jit::resume_stop(Registers &registers, NativeContext &context) {
  constexpr auto key = std::uint64_t{3} << 62U;
  auto found = impl_->blocks.find(key);
  if (found == impl_->blocks.end()) {
    found = impl_->blocks.emplace(key, impl_->arena.append(emit_stop_wake())).first;
    ++impl_->statistics.compiled_entries;
  }
  enter(found->second, registers, context);
}

Statistics Jit::statistics() const noexcept {
  return impl_->statistics;
}
}
