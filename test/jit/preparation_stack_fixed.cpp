// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "preparation_stack.hpp"
#include "stack_model.hpp"

#include <array>
#include <cstddef>
#include <set>
#include <span>
#include <vector>

namespace cpu12::test::prepared_stack {
namespace {
constexpr unsigned stride = 8;
constexpr Byte direct_jsr = 0x17, extended_jsr = 0x16, extended_jmp = 6;
constexpr std::array<Address, 8> targets{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0xfffe, 0xffff};

struct Fixed {
  Byte opcode;
  Address target;
  unsigned length;
};

std::vector<Fixed> operations() {
  std::vector<Fixed> result;
  for (unsigned target = 0; target <= byte_mask; ++target) {
    result.push_back({.opcode = direct_jsr, .target = static_cast<Address>(target), .length = 2});
  }
  for (const auto opcode : {extended_jsr, extended_jmp}) {
    for (const auto target : targets) {
      result.push_back({.opcode = opcode, .target = target, .length = 3});
    }
  }
  return result;
}

Program source(const std::span<const Fixed> operations) {
  Program result;
  result.code.resize(operations.size() * stride, padding);
  std::set<Address> missing;
  for (std::size_t index = 0; index < operations.size(); ++index) {
    const auto point = operations[index];
    const auto offset = index * stride;
    result.code[offset] = point.opcode;
    result.code[offset + 1] =
        static_cast<Byte>(point.length == 2 ? point.target : point.target >> byte_bits);
    if (point.length == 3) { result.code[offset + 2] = static_cast<Byte>(point.target); }
    result.code[offset + point.length] = bra;
    result.code[offset + point.length + 1] = static_cast<Byte>(byte_mask - point.length - 1);
    result.entries.push_back(static_cast<Address>(entry + offset));
    result.entries.push_back(static_cast<Address>(entry + offset + point.length));
    missing.insert(point.target);
  }
  result.nodes = operations.size() * 2;
  result.unresolved = missing.size();
  return result;
}
}

std::size_t fixed_calls() {
  const auto points = operations();
  Pair pair(source(points));
  std::size_t count = 0;
  for (std::size_t index = 0; index < points.size(); ++index) {
    const auto operation = points[index];
    for (auto before : stack::seeds) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        before.pc = static_cast<Address>(entry + (index * stride));
        before.ccr = static_cast<Byte>(ccr);
        pair.seed(before);
        const auto progress = pair.prepared.progress().estimated_cycles;
        auto registers = before;
        registers.pc = operation.target;
        const bool call = operation.opcode != extended_jmp;
        if (call) { registers.sp = static_cast<Address>(static_cast<int>(registers.sp) - 2); }
        const State wanted{.registers = registers, .failure = {}};
        const RunResult expected{.retired = 1, .pc = before.pc, .opcode = operation.opcode};
        const auto actual = pair.prepared.run(1);
        require(
            actual == expected && pair.prepared.state() == wanted &&
                pair.prepared.progress().estimated_cycles == progress + (call ? 4U : 3U) &&
                pair.native.run(1) == actual && pair.native.state() == wanted &&
                pair.native.progress() == pair.prepared.progress(),
            "Direct/extended calls and jumps require exact independently modeled state and timing");
        require(!call || stack::word(pair.optimized.bytes, registers.sp) ==
                             static_cast<Address>(before.pc + operation.length),
                "Every direct call operand stores the correct complete following address");
        pair.retained();
        ++count;
      }
    }
  }
  require(pair.optimized.bytes == pair.reference.bytes,
          "Fixed control transfers preserve complete RAM");
  return count;
}
}
