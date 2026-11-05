// SPDX-License-Identifier: GPL-3.0-only
// The typed SysV bridge and W^X allocation pattern are adapted from the independent
// Weichwolf/dsp56300 project (GPL-3.0-only). No foreign emulator is used.
#include "jit.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/emitter.hpp"
#include "jit/interrupt.hpp"
#include "runtime.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
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
class Executable {
public:
  explicit Executable(const std::span<const std::uint8_t> bytes) {
    const auto page = sysconf(_SC_PAGESIZE);
    if (page <= 0) { throw std::runtime_error("Cannot determine JIT page size"); }
    const auto page_size = static_cast<std::size_t>(page);
    size_ = ((bytes.size() + page_size - 1) / page_size) * page_size;
    memory_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory_ == MAP_FAILED) {
      throw std::system_error(errno, std::generic_category(), "JIT mmap");
    }
    std::memcpy(memory_, bytes.data(), bytes.size());
    if (mprotect(memory_, size_, PROT_READ | PROT_EXEC) != 0) {
      const auto error = errno;
      munmap(memory_, size_);
      throw std::system_error(error, std::generic_category(), "JIT mprotect");
    }
  }

  ~Executable() { munmap(memory_, size_); }

  Executable(const Executable &) = delete;
  Executable &operator=(const Executable &) = delete;

  void enter(Registers &registers, NativeContext &context) const noexcept {
#if defined(__x86_64__) && defined(__linux__)
    cpu12_enter_generated(memory_, &registers, &context);
#else
    static_cast<void>(registers);
    static_cast<void>(context);
#endif
  }

private:
  void *memory_ = nullptr;
  std::size_t size_ = 0;
};
}

class Jit::Impl {
public:
  Ram ram;
  std::size_t cache_entries = 0;
  std::unordered_map<std::uint64_t, std::unique_ptr<Executable>> blocks;
  Statistics statistics;
};

Jit::Jit(const Ram ram, const Options options) : impl_(std::make_unique<Impl>()) {
  constexpr std::size_t maximum_entries = 4096;
  if (options.cache_entries == 0 || options.cache_entries > maximum_entries) {
    throw std::invalid_argument("CPU12 cache_entries must be in [1,4096]");
  }
  impl_->ram = ram;
  impl_->cache_entries = options.cache_entries;
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
    if (impl_->blocks.size() == impl_->cache_entries) { clear(); }
    found = impl_->blocks.emplace(key, std::make_unique<Executable>(emit(*instruction, impl_->ram)))
                .first;
    ++impl_->statistics.compiled_entries;
  }
  found->second->enter(registers, context);
  if (context.fault == Fault::none) {
    ++impl_->statistics.native_instructions;
    if (context.irq_delay != 0) { --context.irq_delay; }
  }
  return true;
}

void Jit::interrupt(const InterruptRequest request, Registers &registers, NativeContext &context) {
  constexpr unsigned kind_shift = 16;
  constexpr auto event_tag = std::uint64_t{1} << 63U;
  const auto key =
      event_tag | (static_cast<std::uint64_t>(request.kind) << kind_shift) | request.vector;
  auto found = impl_->blocks.find(key);
  if (found == impl_->blocks.end()) {
    if (impl_->blocks.size() == impl_->cache_entries) { clear(); }
    found = impl_->blocks
                .emplace(key, std::make_unique<Executable>(emit_interrupt(request, impl_->ram)))
                .first;
    ++impl_->statistics.compiled_entries;
  }
  found->second->enter(registers, context);
}

void Jit::clear() {
  impl_->blocks.clear();
  ++impl_->statistics.cache_resets;
}

Statistics Jit::statistics() const noexcept {
  return impl_->statistics;
}
}
