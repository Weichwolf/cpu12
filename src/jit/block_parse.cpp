// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "immutable.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"

#include <bitset>
#include <optional>
#include <span>
#include <vector>

namespace cpu12::detail {
namespace {
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
  auto window = immutable_bytes(memory, start);
  if (window.empty()) { return {}; }
  Address window_start = start;
  std::vector<BlockInstruction> instructions;
  std::bitset<address_bytes> visited;
  Address pc = start;
  while (!visited[pc]) {
    visited[pc] = true;
    auto offset = static_cast<Address>(pc - window_start);
    if (offset >= window.size()) {
      window = immutable_bytes(memory, pc);
      window_start = pc;
      offset = 0;
    }
    if (window.empty()) { break; }
    auto decoded = decode_first(window.subspan(offset));
    if (!decoded) {
      // A real instruction may cross an immutable grant or the logical wrap.
      decoded = fetch_immutable(memory, pc).instruction;
    }
    if (!decoded || boundary(*decoded)) { break; }
    instructions.push_back({.instruction = *decoded, .pc = pc});
    if (ends(*decoded)) { break; }
    pc = static_cast<Address>(pc + decoded->length);
  }
  return instructions;
}
}
