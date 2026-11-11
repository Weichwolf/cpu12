// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "immutable.hpp"
#include "jit/prefixes.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <span>

namespace cpu12::detail {
namespace {
constexpr auto infinite = std::numeric_limits<std::uint64_t>::max();
constexpr unsigned frame_bytes = 9, byte_bits = 8;
constexpr unsigned mask_i = 0x10, mask_x = 0x40;

bool private_frame(const Memory &memory, const Address stack) {
  if (stack < frame_bytes) { return false; }
  return std::ranges::any_of(memory.regions(), [&](const auto &region) {
    return region.writable && stack >= region.first + frame_bytes &&
           static_cast<unsigned>(stack - region.first) <= region.bytes.size();
  });
}

std::uint64_t plus(const unsigned entry, const std::uint64_t continuation) {
  return continuation > infinite - entry ? infinite : continuation + entry;
}
}

std::uint64_t interrupt_entry_prefix(const State &state,
                                     const Memory &memory,
                                     const std::map<Address, std::uint64_t> &foreground,
                                     const std::span<const InterruptRequest> interrupts) {
  if (state.failure.fault != Fault::none) { return 0; }
  const auto running = state.execution == ExecutionState::running;
  const auto reuse = !running || state.returned_frame;
  auto earliest = infinite;
  for (const auto request : interrupts) {
    if (request.kind != InterruptKind::maskable && request.kind != InterruptKind::xirq) {
      return 0;
    }
    const auto mask = request.kind == InterruptKind::xirq ? mask_x : mask_i;
    if ((state.registers.ccr & mask) != 0) { continue; }
    // Ignoring a remaining IRQ deferral admits more paths, never a later bound.
    const auto vector = immutable_bytes(memory, request.vector);
    if (vector.size() < 2 || (!reuse && !private_frame(memory, state.registers.sp))) { return 0; }
    const auto target = static_cast<Address>((unsigned{vector[0]} << byte_bits) | vector[1]);
    const auto found = foreground.find(target);
    if (found == foreground.end()) { return 0; }
    auto entered = state;
    entered.registers.pc = target;
    entered.registers.ccr = static_cast<Byte>(entered.registers.ccr | mask_i | mask);
    if (running) {
      entered.registers.sp =
          static_cast<Address>(unsigned{entered.registers.sp} + address_bytes - frame_bytes);
    }
    entered.execution = ExecutionState::running;
    entered.irq_delay = 0;
    entered.returned_frame = false;
    const auto after =
        std::min(found->second, interrupt_entry_prefix(entered, memory, foreground, interrupts));
    earliest = std::min(earliest, plus(reuse ? 6U : 9U, after));
  }
  return earliest;
}
}
