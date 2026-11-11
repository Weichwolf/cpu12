// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_callbacks_support.hpp"
#include "image_support.hpp"
#include "isa.hpp"
#include "jit/llvm/callback_binding.hpp"
#include "jit/llvm/image.hpp"
#include "public_io.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cpu12::test::image {
namespace {
constexpr std::uint64_t id = 7;
constexpr Address port = 0xffff, idle = entry + 7;
constexpr Byte mask = 0x80, immediate = 0x55, retained_flags = 0xf1;
constexpr Registers initial{.pc = entry, .d = 0x5aa5, .x = 0x1357, .y = 0x2468, .sp = 0x9000};
constexpr auto code = isa::program(isa::opcode<isa::Opcode::brset_extended>(),
                                   std::array<Byte, 4>{0xff, 0xff, mask, 2},
                                   isa::opcode<isa::Opcode::ldaa_immediate>(),
                                   std::array<Byte, 1>{immediate},
                                   std::array<Byte, 2>{bra, backedge});

enum class Mode : std::uint8_t { all, read_fallback, native_masked };

struct Point {
  Byte value, flags;
  unsigned quota;
  bool failing, yielding;
  Mode mode;
};

struct Expected {
  Registers registers = initial;
  std::uint64_t cycles = 0;
  unsigned retired = 0;
};

Expected predict(const Point point) {
  Expected result;
  result.registers.ccr = point.flags;
  if (point.quota == 0 || point.failing) { return result; }
  const bool taken = (point.value & mask) != 0;
  result.registers.pc = taken ? idle : entry + 5;
  result.cycles = 5;
  result.retired = 1;
  if (point.yielding) { return result; }
  if (!taken && point.quota >= 2) {
    result.registers.pc = idle;
    result.registers.d =
        static_cast<Address>((unsigned{immediate} << byte_bits) | (initial.d & byte_mask));
    result.registers.ccr &= retained_flags;
    ++result.cycles;
    ++result.retired;
  }
  result.cycles += std::uint64_t{point.quota - result.retired} * 3;
  result.retired = point.quota;
  return result;
}

void prefix(const detail::llvm19::ImageProgram &image, const Point point) {
  auto executable = code;
  const std::array grants{Ram{.first = entry, .bytes = executable, .writable = false}};
  Owner owner(image, id, Memory::mapped(grants));
  auto before = initial;
  before.ccr = point.flags;
  owner.seed(before);
  public_io::Bus bus;
  bus.memory[port] = point.value;
  bus.state = &owner.context.cpu;
  bus.progress = &owner.context.progress;
  if (point.failing) { bus.fail_at = 0; }
  if (point.yielding) {
    bus.observer = &owner.context;
    bus.notify = [](void *context) noexcept {
      static_cast<detail::NativeContext *>(context)->yield_requested = true;
    };
  }
  owner.context.io.owner = &bus;
  if (point.mode == Mode::native_masked) { owner.context.io.read_masked = fixture_read_masked; }
  const auto memory = bus.memory;
  const auto expected = predict(point);
  const auto result = owner.run(point.quota);
  const bool accessed = point.quota != 0;
  const bool failed = accessed && point.failing;
  require(
      result.retired == expected.retired &&
          owner.context.cpu == State{.registers = expected.registers, .failure = {}} &&
          owner.context.progress == Progress{.estimated_cycles = expected.cycles} &&
          owner.context.yield_requested == (accessed && point.yielding) &&
          bus.calls == (accessed ? 1U : 0U) && bus.memory == memory &&
          owner.context.fault == (failed ? Fault::memory_read : Fault::none) &&
          owner.context.address == (failed ? port : 0) &&
          owner.context.value == (failed ? point.value : 0) &&
          owner.context.access_size == (failed ? 1 : 0) &&
          owner.context.read_mask == (failed ? mask : byte_mask),
      "Public masked reads preserve selected values, full failure bytes, yield and branch timing");
  if (accessed) {
    const public_io::Access wanted{
        .address = port,
        .value = point.value,
        .width = 1,
        .mask = point.mode == Mode::read_fallback ? static_cast<Byte>(byte_mask) : mask,
        .state = {.registers = before, .failure = {}},
        .progress = {}};
    require(bus.trace[0] == wanted,
            "Selected and runtime fallback roles expose the actual public mask ABI and live state");
  }
  if (accessed && point.yielding && !point.failing) {
    auto continuation = point;
    continuation.yielding = false;
    continuation.quota = 4;
    const auto wanted = predict(continuation);
    owner.context.yield_requested = false;
    require(owner.run(3).retired == 3 &&
                owner.context.cpu == State{.registers = wanted.registers, .failure = {}} &&
                owner.context.progress == Progress{.estimated_cycles = wanted.cycles} &&
                owner.context.fault == Fault::none && !owner.context.yield_requested &&
                bus.calls == 1 && bus.memory == memory,
            "A later native grant resumes the original masked branch without replaying I/O");
  }
}

void masked_cases(const detail::llvm19::ImageProgram &image, const Mode mode) {
  constexpr std::array<Byte, 8> flags{0, 1, 0x0e, 0x20, 0x55, 0x80, 0xc1, 0xff};
  constexpr std::array<unsigned, 4> quotas{0, 1, 2, 5};
  for (unsigned value = 0; value <= byte_mask; ++value) {
    for (const auto ccr : flags) {
      for (const auto quota : quotas) {
        for (const bool failing : {false, true}) {
          for (const bool yielding : {false, true}) {
            prefix(image,
                   {.value = static_cast<Byte>(value),
                    .flags = ccr,
                    .quota = quota,
                    .failing = failing,
                    .yielding = yielding,
                    .mode = mode});
          }
        }
      }
    }
  }
}
}

void callback_masked(const std::span<const std::byte> bindings,
                     const std::span<const detail::llvm19::CallbackBinding> callbacks) {
  const std::array spans{CodeSpan{.address = entry, .words = code}};
  constexpr std::array roots{entry};
  const std::array variants{CodeVariant{.id = id, .code = spans, .entries = roots}};
  constexpr std::array regions{
      RegionLayout{.first = entry, .size = code.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = id, .memory = regions}};
  const detail::llvm19::ImageProgram all(
      {.variants = variants}, layouts, bindings, {}, {}, callbacks);
  const detail::llvm19::ImageProgram fallback(
      {.variants = variants}, layouts, bindings, {}, {}, callbacks.first(1));
  masked_cases(all, Mode::all);
  masked_cases(fallback, Mode::read_fallback);
  masked_cases(fallback, Mode::native_masked);
}
}
