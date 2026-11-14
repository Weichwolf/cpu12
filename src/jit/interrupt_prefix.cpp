// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "immutable.hpp"
#include "instruction.hpp"
#include "jit/prefixes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>

namespace cpu12::detail {
namespace {
constexpr auto infinite = std::numeric_limits<std::uint64_t>::max();
constexpr unsigned frame_bytes = 9, byte_bits = 8;
constexpr unsigned mask_i = 0x10, mask_x = 0x40;

std::uint64_t plus(const unsigned entry, const std::uint64_t continuation) {
  return continuation > infinite - entry ? infinite : continuation + entry;
}

struct StorageAccess {
  Address first;
  unsigned width;
  bool write;
};

bool private_storage(const Memory &memory, const StorageAccess access) {
  return std::ranges::any_of(memory.regions(), [&](const auto &region) {
    if (access.first < region.first || (access.write && !region.writable)) { return false; }
    const auto offset = static_cast<std::size_t>(access.first - region.first);
    return offset <= region.bytes.size() && access.width <= region.bytes.size() - offset &&
           (access.write || region.known.empty() ||
            std::ranges::all_of(region.known.subspan(offset, access.width),
                                [](const auto byte) { return byte != 0; }));
  });
}
}

bool private_frame(const Memory &memory, const Address stack) {
  return stack >= frame_bytes &&
         private_storage(memory,
                         {.first = static_cast<Address>(stack - frame_bytes),
                          .width = frame_bytes,
                          .write = true});
}

bool private_return_frame(const Memory &memory, const Address stack) {
  return private_storage(memory, {.first = stack, .width = frame_bytes, .write = false});
}

std::uint64_t private_operation_prefix(const Instruction &instruction,
                                       const Memory &memory,
                                       const Address stack) {
  if (instruction.indexed.indirect) { return 0; }
  const auto cycles = instruction_timing(instruction).base;
  if (!may_access_owner(instruction, memory)) { return cycles; }
  bool write = false;
  unsigned width = instruction.word ? 2 : 1;
  if (instruction.kind == Kind::call) {
    if (instruction.indexed.base == IndexBase::sp && instruction.indexed.update != 0) { return 0; }
    write = true;
    width = 2;
  } else if (instruction.kind == Kind::push) {
    write = true;
  } else if (instruction.kind != Kind::pull && instruction.kind != Kind::return_subroutine) {
    return 0;
  }
  if (write && stack < width) { return 0; }
  const auto address = static_cast<Address>(write ? stack - width : stack);
  return private_storage(memory, {.first = address, .width = width, .write = write}) ? cycles : 0;
}

std::uint64_t isolated_store_prefix(const Instruction &instruction,
                                    const Registers &registers,
                                    const std::span<const ByteWrite> isolated) {
  if (instruction.kind != Kind::store || instruction.word ||
      (instruction.addressing != Addressing::direct &&
       instruction.addressing != Addressing::extended)) {
    return 0;
  }
  const auto value =
      static_cast<Byte>(instruction.accumulator_b ? registers.d : registers.d >> byte_bits);
  return std::ranges::any_of(isolated,
                             [&](const auto write) {
                               return write.address == instruction.address && write.value == value;
                             })
             ? instruction_timing(instruction).base
             : 0;
}

std::uint64_t interrupt_entry_prefix(const State &state,
                                     const Memory &memory,
                                     const PrefixLookup foreground,
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
    const auto found = foreground.find(foreground.owner, target);
    if (!found) { return 0; }
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
        std::min(*found, interrupt_entry_prefix(entered, memory, foreground, interrupts));
    earliest = std::min(earliest, plus(reuse ? 6U : 9U, after));
  }
  return earliest;
}

std::uint64_t interrupt_entry_prefix(const State &state,
                                     const Memory &memory,
                                     const std::map<Address, std::uint64_t> &foreground,
                                     const std::span<const InterruptRequest> interrupts) {
  const PrefixLookup lookup{
      .owner = &foreground,
      .find = [](const void *owner, const Address pc) -> std::optional<std::uint64_t> {
        const auto &prefixes = *static_cast<const std::map<Address, std::uint64_t> *>(owner);
        const auto found = prefixes.find(pc);
        return found == prefixes.end() ? std::nullopt : std::optional{found->second};
      }};
  return interrupt_entry_prefix(state, memory, lookup, interrupts);
}
}
