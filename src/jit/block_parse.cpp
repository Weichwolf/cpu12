// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "runtime.hpp"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cpu12::detail {
namespace {
std::span<const Byte> permanent_bytes(const Memory &memory, const Address address) {
  for (const auto &region : memory.regions()) {
    if (region.writable || !region.known.empty() || address < region.first) { continue; }
    const auto offset = static_cast<std::size_t>(address - region.first);
    if (offset >= region.bytes.size()) { continue; }
    const auto *pointer = region.bytes.data() + offset;
    auto remaining = region.bytes.size() - offset;
    const auto physical = reinterpret_cast<std::uintptr_t>(pointer);
    for (const auto &alias : memory.regions()) {
      if (!alias.writable || alias.bytes.empty()) { continue; }
      const auto first = reinterpret_cast<std::uintptr_t>(alias.bytes.data());
      if (physical >= first && physical - first < alias.bytes.size()) { return {}; }
      if (first > physical && first - physical < remaining) {
        remaining = static_cast<std::size_t>(first - physical);
      }
    }
    return {pointer, remaining};
  }
  return {};
}

std::optional<Instruction> capture_permanent(const Memory &memory, const Address pc) {
  NativeContext context;
  const auto capture = fetch(memory, context, pc);
  if (!capture.supported || context.fault != Fault::none) { return std::nullopt; }
  for (std::size_t index = 0; index < capture.size; ++index) {
    if (permanent_bytes(memory, static_cast<Address>(pc + index)).empty()) { return std::nullopt; }
  }
  return decode(std::span(capture.bytes).first(capture.size));
}

bool boundary(const Instruction &instruction) {
  return instruction.kind == Kind::software_interrupt ||
         instruction.kind == Kind::return_interrupt || instruction.kind == Kind::transfer ||
         instruction.kind == Kind::pull ||
         (instruction.kind == Kind::register_operation &&
          (instruction.operation == RegisterOp::ccr_and ||
           instruction.operation == RegisterOp::ccr_or));
}

bool ends(const Instruction &instruction) {
  return instruction.kind == Kind::branch || instruction.kind == Kind::bit_branch ||
         instruction.kind == Kind::loop || instruction.kind == Kind::jump ||
         instruction.kind == Kind::call || instruction.kind == Kind::return_subroutine ||
         instruction.kind == Kind::push || instruction.kind == Kind::move ||
         instruction.kind == Kind::wait || instruction.kind == Kind::stop;
}

}

std::vector<BlockInstruction> parse_block(const Memory &memory, const Address start) {
  auto window = permanent_bytes(memory, start);
  if (window.empty()) { return {}; }
  Address window_start = start;
  std::vector<BlockInstruction> instructions;
  std::bitset<address_bytes> visited;
  Address pc = start;
  while (!visited[pc]) {
    visited[pc] = true;
    auto offset = static_cast<Address>(pc - window_start);
    if (offset >= window.size()) {
      window = permanent_bytes(memory, pc);
      window_start = pc;
      offset = 0;
    }
    if (window.empty()) { break; }
    auto decoded = decode_first(window.subspan(offset));
    if (!decoded) {
      // A real instruction may cross an immutable grant or the logical wrap.
      decoded = capture_permanent(memory, pc);
    }
    if (!decoded || boundary(*decoded)) { break; }
    instructions.push_back({.instruction = *decoded, .pc = pc});
    if (ends(*decoded)) { break; }
    pc = static_cast<Address>(pc + decoded->length);
  }
  return instructions;
}
}
