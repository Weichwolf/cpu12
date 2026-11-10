// SPDX-License-Identifier: GPL-3.0-only
#include "jit/preparation.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"

#include <array>
#include <bitset>
#include <queue>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::detail {
Successors successors(const BlockInstruction &operation) {
  const auto &instruction = operation.instruction;
  const auto next = static_cast<Address>(operation.pc + instruction.length);
  const auto relative = static_cast<Address>(next + instruction.operand);
  switch (instruction.kind) {
    case Kind::branch:
      if (instruction.condition == 0) { return {.direct = {relative}, .count = 1}; }
      if (instruction.condition == 1) { return {.direct = {next}, .count = 1}; }
      return {.direct = {next, relative}, .count = 2};
    case Kind::bit_branch:
    case Kind::loop: return {.direct = {next, relative}, .count = 2};
    case Kind::call:
    case Kind::jump: {
      Successors result;
      if (instruction.kind == Kind::call) { result.direct[result.count++] = next; }
      if (instruction.addressing == Addressing::extended ||
          instruction.addressing == Addressing::direct) {
        result.direct[result.count++] = instruction.address;
      } else if (instruction.addressing == Addressing::relative) {
        result.direct[result.count++] = relative;
      } else {
        result.indirect = true;
      }
      return result;
    }
    case Kind::pull:
      return instruction.stack_target == StackTarget::pc ? Successors{.indirect = true}
                                                         : Successors{.direct = {next}, .count = 1};
    case Kind::return_subroutine:
    case Kind::return_interrupt: return {.indirect = true};
    case Kind::software_interrupt:
    case Kind::wait:
    case Kind::stop: return {.direct = {next}, .count = 1, .indirect = true};
    case Kind::nop:
    case Kind::load:
    case Kind::add:
    case Kind::add_carry:
    case Kind::subtract:
    case Kind::subtract_carry:
    case Kind::compare:
    case Kind::bit_and:
    case Kind::bit_or:
    case Kind::bit_xor:
    case Kind::bit_test:
    case Kind::store:
    case Kind::effective_address:
    case Kind::transfer:
    case Kind::push:
    case Kind::register_operation:
    case Kind::memory_unary:
    case Kind::memory_bits:
    case Kind::move:
    case Kind::multiply_divide: return {.direct = {next}, .count = 1};
    default: throw std::logic_error("Invalid CPU12 preparation instruction kind");
  }
}

PreparationGraph preparation_graph(const Memory &memory,
                                   const std::span<const Address> established_entries) {
  PreparationGraph result;
  std::queue<Address> pending;
  std::bitset<address_bytes> visited;
  const auto enqueue = [&](const Address address) {
    if (visited[address]) { return; }
    visited[address] = true;
    pending.push(address);
  };
  for (const auto entry : established_entries) { enqueue(entry); }
  while (!pending.empty()) {
    const auto address = pending.front();
    pending.pop();
    const auto capture = fetch_immutable(memory, address);
    if (capture.failure.fault == Fault::instruction_fetch) {
      result.unresolved.push_back(address);
      continue;
    }
    if (capture.failure.fault != Fault::none) {
      result.failure = capture.failure;
      return result;
    }
    const BlockInstruction operation{.instruction = *capture.instruction, .pc = address};
    result.instructions.push_back(operation);
    const auto flow = successors(operation);
    if (flow.indirect) { result.unresolved.push_back(address); }
    for (const auto target : std::span(flow.direct).first(flow.count)) { enqueue(target); }
  }
  return result;
}
}
