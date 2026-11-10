// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "preparation_stack.hpp"
#include "stack_model.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace cpu12::test::prepared_stack {
namespace {
constexpr unsigned stride = 4;
constexpr Byte left_guard = 0xa5, right_guard = 0x5a;
constexpr Address last_address = 0xffff, ordinary_stack = 0x9000;
constexpr std::array<Address, 6> positions{0, 1, 2, 0x7fff, 0xfffe, 0xffff};
constexpr std::array<Address, 8> values{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff};

Program program() {
  Program result;
  result.code.resize(stack::operations.size() * stride, padding);
  for (std::size_t index = 0; index < stack::operations.size(); ++index) {
    const auto offset = index * stride;
    result.code[offset] = stack::operations[index].opcode;
    result.code[offset + 1] = bra;
    result.code[offset + 2] = byte_mask - 1;
    result.entries.push_back(static_cast<Address>(entry + offset));
    result.entries.push_back(static_cast<Address>(entry + offset + 1));
  }
  result.nodes = stack::operations.size() * 2;
  result.unresolved = 1;
  return result;
}

void one(Pair &pair, const std::size_t index, Registers before, Address operand) {
  const auto operation = stack::operations[index];
  before.pc = static_cast<Address>(entry + (index * stride));
  if (operation.width == 1) { operand &= byte_mask; }
  const auto wanted = stack::expected(operation, before, operand);
  const auto address = operation.push ? wanted.sp : before.sp;
  const auto left = static_cast<Address>(static_cast<int>(address) - 1);
  const auto right = static_cast<Address>(address + operation.width);
  for (auto *fixture : {&pair.optimized, &pair.reference}) {
    fixture->bytes[left] = left_guard;
    fixture->bytes[right] = right_guard;
    if (operation.width == 2) {
      stack::word(fixture->bytes, address, operand);
    } else {
      fixture->bytes[address] = static_cast<Byte>(operand);
    }
  }
  pair.seed(before);
  const auto progress = pair.prepared.progress().estimated_cycles;
  const RunResult expected{.retired = 1, .pc = before.pc, .opcode = operation.opcode};
  const State state{.registers = wanted, .failure = {}};
  const auto actual = pair.prepared.run(1);
  const auto pull_cycles = operation.target == stack::Target::pc ? 5U : 3U;
  const auto cycles = operation.push ? 2U : pull_cycles;
  if (actual != expected || pair.prepared.state() != state ||
      pair.prepared.progress().estimated_cycles != progress + cycles) {
    throw std::runtime_error("LLVM stack opcode=" + std::to_string(operation.opcode) + " SP=" +
                             std::to_string(before.sp) + " CCR=" + std::to_string(before.ccr));
  }
  require(pair.native.run(1) == actual && pair.native.state() == state &&
              pair.native.progress() == pair.prepared.progress(),
          "Independent LLVM stack state and timing also match ordinary native semantics");
  const auto output = operation.push ? stack::value(before, operation.target) : operand;
  const auto actual_word = operation.width == 2 ? stack::word(pair.optimized.bytes, address)
                                                : pair.optimized.bytes[address];
  require(actual_word == output && pair.optimized.bytes[left] == left_guard &&
              pair.optimized.bytes[right] == right_guard &&
              pair.optimized.bytes[address] == pair.reference.bytes[address] &&
              pair.optimized.bytes[static_cast<Address>(address + 1U)] ==
                  pair.reference.bytes[static_cast<Address>(address + 1U)],
          "LLVM stack accesses preserve wrapped big-endian bytes and adjacent guards");
  pair.retained();
}

std::size_t matrix(Pair &pair) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < stack::operations.size(); ++index) {
    for (auto before : stack::seeds) {
      for (const auto sp : positions) {
        before.sp = sp;
        for (const auto operand : values) {
          one(pair, index, before, operand);
          require(pair.optimized.bytes == pair.reference.bytes,
                  "Every modeled stack boundary preserves complete RAM");
          ++count;
        }
      }
    }
  }
  return count;
}

std::size_t full_domains(Pair &pair) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < stack::operations.size(); ++index) {
    const auto operation = stack::operations[index];
    const auto limit = operation.width == 2 ? address_bytes : byte_mask + 1U;
    for (unsigned value = 0; value < limit; ++value) {
      auto before = stack::seeds[0];
      before.ccr = static_cast<Byte>(value);
      if (operation.push) { stack::assign(before, operation.target, static_cast<Address>(value)); }
      before.sp = operation.push ? 1 : last_address;
      one(pair, index, before, static_cast<Address>(value));
      ++count;
    }
  }
  constexpr std::size_t pulc = 8;
  for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
    for (unsigned value = 0; value <= byte_mask; ++value) {
      auto before = stack::seeds[0];
      before.sp = ordinary_stack;
      before.ccr = static_cast<Byte>(ccr);
      one(pair, pulc, before, static_cast<Address>(value));
      ++count;
    }
  }
  require(pair.optimized.bytes == pair.reference.bytes,
          "Complete full-domain stack RAM must agree");
  return count;
}
}
}

int main() {
  try {
    using namespace cpu12::test::prepared_stack;
    Pair pair(program());
    const auto count = matrix(pair) + full_domains(pair) + calls() + fixed_calls();
    boundaries();
    std::cout << "Verified " << count
              << " independently modeled LLVM stack/call/jump cases, "
                 "nested returns, deadlines, precise I/O and retained native code.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
