// SPDX-License-Identifier: GPL-3.0-only
#include "jit/block.hpp"

#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/a64/emitter.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::detail::a64 {
namespace {
Address following(const BlockInstruction &operation) {
  return static_cast<Address>(operation.pc + operation.instruction.length);
}

Address target(const BlockInstruction &operation) {
  const auto &instruction = operation.instruction;
  if (instruction.kind == Kind::branch || instruction.kind == Kind::loop) {
    return static_cast<Address>(following(operation) + instruction.operand);
  }
  if (instruction.kind == Kind::jump && instruction.addressing == Addressing::extended) {
    return instruction.address;
  }
  return following(operation);
}

std::vector<BlockInstruction> loop_tail(const std::span<const BlockInstruction> operations,
                                        const Memory &memory) {
  const auto &last = operations.back();
  if (last.instruction.kind != Kind::branch || last.instruction.condition < 2) { return {}; }
  auto tail = parse_block(memory, following(last));
  if (tail.empty() || target(last) != following(tail.back()) ||
      target(tail.back()) != operations.front().pc) {
    return {};
  }
  const auto &backedge = tail.back().instruction;
  if ((backedge.kind == Kind::branch && backedge.condition == 0) ||
      (backedge.kind == Kind::jump && backedge.addressing == Addressing::extended)) {
    return tail;
  }
  return {};
}
}

void Emitter::sequence(const std::span<const BlockInstruction> operations) {
  for (const auto &item : operations) {
    code_.branch_zero_far(remaining, done_);
    location(item.instruction);
    operation(item.instruction);
    code_.add(retired, retired, 1);
    code_.constant(Reg::x17, 1);
    code_.alu(Alu::subtract, remaining, remaining, Reg::x17);
  }
}

std::vector<Byte> Emitter::block(const std::span<const BlockInstruction> operations) {
  if (operations.empty()) { throw std::invalid_argument("Cannot emit an empty A64 block"); }
  const auto tail = loop_tail(operations, memory_);
  const bool cyclic = !tail.empty() || target(operations.back()) == operations.front().pc;
  begin(true);
  const auto &last = operations.back();
  const bool boundary = last.instruction.kind == Kind::wait || last.instruction.kind == Kind::stop;
  code_.constant(Reg::x17,
                 std::uint32_t{last.pc} |
                     (std::uint32_t{last.instruction.opcode} << native_opcode_shift) |
                     (boundary ? native_boundary_mask : 0U));
  constexpr unsigned location_offset = 24;
  code_.store(Reg::x17, Reg::sp, location_offset);
  if (!cyclic) {
    code_.constant(Reg::x17, operations.size());
    code_.compare(remaining, Reg::x17);
    code_.branch_far(Condition::no_carry, done_);
  }
  Label repeat;
  code_.bind(repeat);
  sequence(operations);
  if (cyclic && !boundary) {
    code_.constant(Reg::x17, operations.front().pc);
    code_.compare(pc, Reg::x17);
    code_.branch_far(Condition::equal, repeat);
    if (!tail.empty()) {
      code_.constant(Reg::x17, tail.front().pc);
      code_.compare(pc, Reg::x17);
      code_.branch_far(Condition::not_equal, done_);
      sequence(tail);
      code_.branch(repeat);
    }
  }
  return finish();
}
}
