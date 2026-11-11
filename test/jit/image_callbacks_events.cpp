// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_callbacks_support.hpp"
#include "jit/interrupt.hpp"
#include "jit/llvm/image.hpp"
#include "jit/llvm/program.hpp"
#include "public_io.hpp"
#include "runtime.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace cpu12::test::image {
namespace {
constexpr Address vector = 0xfff0, handler = 0x4100;
constexpr unsigned no_failure = std::numeric_limits<unsigned>::max();
constexpr std::uint64_t id = 7;
constexpr unsigned frame_bytes = 9;
constexpr unsigned writes = 5;
constexpr Registers initial{.pc = entry, .d = 0x5aa5, .x = 0x1357, .y = 0x2468};
using Event = void (*)(Registers *, detail::NativeContext *) noexcept;

struct Point {
  Address sp;
  Byte flags;
  unsigned failure, yielding;
};

struct Yield {
  detail::NativeContext &context;
  const public_io::Bus &bus;
  unsigned call;

  static void notify(void *owner) noexcept {
    auto &self = *static_cast<Yield *>(owner);
    if (self.bus.calls == self.call) { self.context.yield_requested = true; }
  }
};

std::array<public_io::Access, writes + 1> transactions(const State &state) {
  const auto at = [&](const int distance) {
    return static_cast<Address>(int{state.registers.sp} - distance);
  };
  constexpr Address swapped_d = 0xa55a;
  return {{{.address = vector, .value = handler, .width = 2, .state = state, .progress = {}},
           {.address = at(2),
            .value = entry,
            .width = 2,
            .write = true,
            .state = state,
            .progress = {}},
           {.address = at(4),
            .value = initial.y,
            .width = 2,
            .write = true,
            .state = state,
            .progress = {}},
           {.address = at(6),
            .value = initial.x,
            .width = 2,
            .write = true,
            .state = state,
            .progress = {}},
           {.address = at(8),
            .value = swapped_d,
            .width = 2,
            .write = true,
            .state = state,
            .progress = {}},
           {.address = at(9),
            .value = state.registers.ccr,
            .width = 1,
            .write = true,
            .state = state,
            .progress = {}}}};
}

void prefix(const Event code, const Point point) {
  detail::NativeContext owner{};
  owner.cpu.registers = initial;
  owner.cpu.registers.sp = point.sp;
  owner.cpu.registers.ccr = point.flags;
  public_io::Bus bus;
  bus.memory[vector] = static_cast<Byte>(handler >> byte_bits);
  bus.memory[vector + 1] = static_cast<Byte>(handler);
  bus.state = &owner.cpu;
  bus.progress = &owner.progress;
  bus.fail_at = point.failure;
  Yield yield{.context = owner, .bus = bus, .call = point.yielding};
  bus.observer = &yield;
  bus.notify = Yield::notify;
  owner.io.owner = &bus;
  auto wanted = bus;
  const auto accesses = transactions(owner.cpu);
  const unsigned calls = point.failure < accesses.size() ? point.failure + 1 : accesses.size();
  for (unsigned i = 0; i < calls; ++i) {
    const auto access = accesses[i];
    wanted.trace[i] = access;
    if (!access.write) { continue; }
    wanted.memory[access.address] = static_cast<Byte>(access.value);
    if (access.width == 2) {
      wanted.memory[access.address] = static_cast<Byte>(access.value >> byte_bits);
      wanted.memory[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value);
    }
  }
  auto expected = owner.cpu;
  if (point.failure >= accesses.size()) {
    expected.registers.sp = static_cast<Address>(int{point.sp} - int{frame_bytes});
    expected.registers.pc = handler;
    expected.registers.ccr |= 0x10;
  }
  code(&owner.cpu.registers, &owner);
  require(
      owner.cpu == expected && owner.progress == Progress{} && bus.calls == calls &&
          bus.memory == wanted.memory && bus.trace == wanted.trace &&
          owner.yield_requested == (point.yielding < calls),
      "Public fused IRQ callbacks preserve wrapping frame stores, full state and prefix effects");
  if (point.failure < accesses.size()) {
    const auto failure = accesses[point.failure];
    require(owner.fault == (failure.write ? Fault::memory_write : Fault::memory_read) &&
                owner.address == failure.address && owner.value == failure.value &&
                owner.access_size == failure.width && owner.read_mask == byte_mask,
            "Public fused IRQ failure retains the partial callback value and exact access context");
  } else {
    require(owner.fault == Fault::none && owner.address == 0 && owner.value == 0 &&
                owner.access_size == 0 && owner.read_mask == byte_mask,
            "Public fused IRQ success adds no fault state");
  }
}
}

void callback_events(const detail::llvm19::ImageProgram &image) {
  constexpr InterruptRequest request{.vector = vector};
  const auto key = detail::event_key(request, detail::InterruptFrame::push);
  const auto code = std::bit_cast<Event>(image.program(id).event(key));
  require(code != nullptr, "Public callback image retains its native IRQ adapter");
  constexpr std::array<Address, 2> stacks{1, 0x9000};
  constexpr std::array<unsigned, writes + 2> boundaries{0, 1, 2, 3, 4, 5, no_failure};
  for (const auto sp : stacks) {
    for (unsigned flags = 0; flags <= byte_mask; ++flags) {
      for (const auto failure : boundaries) {
        for (const auto yielding : boundaries) {
          prefix(code,
                 {.sp = sp,
                  .flags = static_cast<Byte>(flags),
                  .failure = failure,
                  .yielding = yielding});
        }
      }
    }
  }
}
}
