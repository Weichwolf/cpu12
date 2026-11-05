// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cpu12::detail {
namespace {
const Byte *permanent_byte(const Memory &memory, const Address address) {
  for (const auto &region : memory.regions()) {
    if (region.writable || !region.known.empty() || address < region.first) { continue; }
    const auto offset = static_cast<std::size_t>(address - region.first);
    if (offset >= region.bytes.size()) { continue; }
    const auto *pointer = region.bytes.data() + offset;
    const auto physical = reinterpret_cast<std::uintptr_t>(pointer);
    for (const auto &alias : memory.regions()) {
      if (!alias.writable || alias.bytes.empty()) { continue; }
      const auto first = reinterpret_cast<std::uintptr_t>(alias.bytes.data());
      if (physical >= first && physical - first < alias.bytes.size()) { return nullptr; }
    }
    return pointer;
  }
  return nullptr;
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
  if (permanent_byte(memory, start) == nullptr) { return {}; }
  std::vector<BlockInstruction> instructions;
  std::array<bool, address_bytes> visited{};
  Address pc = start;
  while (!visited[pc]) {
    visited[pc] = true;
    NativeContext context;
    const auto capture = fetch(memory, context, pc);
    if (!capture.supported || context.fault != Fault::none) { break; }
    bool permanent = true;
    for (std::size_t index = 0; index < capture.size; ++index) {
      const auto *pointer = permanent_byte(memory, static_cast<Address>(pc + index));
      if (pointer == nullptr) {
        permanent = false;
        break;
      }
    }
    if (!permanent) { break; }
    const auto decoded = decode(std::span(capture.bytes).first(capture.size));
    if (!decoded || boundary(*decoded)) { break; }
    instructions.push_back({.instruction = *decoded, .pc = pc});
    if (ends(*decoded)) { break; }
    pc = static_cast<Address>(pc + decoded->length);
  }
  return instructions;
}
}
