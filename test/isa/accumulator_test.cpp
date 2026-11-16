// SPDX-License-Identifier: GPL-3.0-only
#include "accumulator_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction_image.hpp"
#include "isa.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <vector>

using cpu12::test::InstructionCore;

namespace {
using namespace cpu12;
using namespace cpu12::test::accumulator;

void full_domain(InstructionCore &core, const Operation operation) {
  const auto code = encode(operation);
  for (unsigned data = 0; data <= word_mask; ++data) {
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      auto before = seed;
      before.d = static_cast<std::uint16_t>(data);
      before.ccr = static_cast<std::uint8_t>(ccr);
      const auto wanted = arithmetic({.operation = operation, .before = before},
                                     {.left = data >> byte_bits, .right = data & byte_mask});
      core.set_registers(before);
      retired(core, code, before, wanted);
    }
  }
}

void pc_wrap(InstructionCore &core) {
  constexpr std::array<std::uint16_t, 4> pcs{0, 1, 0xfffe, 0xffff};
  constexpr std::array<Operation, 3> operations{
      Operation::add, Operation::subtract, Operation::compare};
  constexpr std::array<unsigned, 8> values{0, 1, 0x7fff, 0x8000, 0xffff, 0x00ff, 0x7f01, 0x80ff};
  for (const auto operation : operations) {
    for (const auto pc : pcs) {
      for (const auto data : values) {
        auto before = seed;
        before.pc = pc;
        before.d = static_cast<std::uint16_t>(data);
        const auto wanted = arithmetic({.operation = operation, .before = before},
                                       {.left = data >> byte_bits, .right = data & byte_mask});
        core.set_registers(before);
        retired(core, encode(operation), before, wanted);
      }
    }
  }
}

void aliases(InstructionCore &core) {
  constexpr std::array<std::uint16_t, 8> bases{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xfffe, 0xffff};
  constexpr std::array<unsigned, 4> flags{0, 0xff, 0xa5, 0x5a};
  constexpr std::array instructions{isa::opcode<isa::Opcode::abx_indexed>(),
                                    isa::opcode<isa::Opcode::aby_indexed>()};
  for (unsigned index = 0; index < instructions.size(); ++index) {
    const auto code = instructions[index];
    const auto field = index == 0 ? &Registers::x : &Registers::y;
    for (const auto base : bases) {
      for (unsigned b = 0; b <= byte_mask; ++b) {
        for (const auto ccr : flags) {
          auto before = seed;
          before.d = static_cast<std::uint16_t>(b);
          before.ccr = static_cast<std::uint8_t>(ccr);
          before.*field = base;
          auto wanted = before;
          wanted.*field = static_cast<std::uint16_t>(base + b);
          wanted.pc = static_cast<std::uint16_t>(before.pc + code.size());
          core.set_registers(before);
          retired(core, code, before, wanted);
        }
      }
    }
  }
}

}

int main() {
  try {
    std::vector<std::vector<Byte>> operations;
    for (const auto operation : {Operation::add, Operation::subtract, Operation::compare}) {
      const auto bytes = encode(operation);
      operations.emplace_back(bytes.begin(), bytes.end());
    }
    for (const auto bytes : {isa::opcode<isa::Opcode::abx_indexed>(),
                             isa::opcode<isa::Opcode::aby_indexed>(),
                             isa::opcode<isa::Opcode::daa>()}) {
      operations.emplace_back(bytes.begin(), bytes.end());
    }
    constexpr unsigned decimal_values = 100;
    for (unsigned right = 0; right < decimal_values; ++right) {
      const auto operand = static_cast<Byte>(((right / 10U) * 16U) + (right % 10U));
      operations.push_back({static_cast<Byte>(isa::Opcode::adca_immediate), operand});
    }
    constexpr std::array<Address, 4> positions{0, 1, 0xfffe, 0xffff};
    InstructionCore core(operations, positions);
    full_domain(core, Operation::add);
    full_domain(core, Operation::subtract);
    full_domain(core, Operation::compare);
    pc_wrap(core);
    aliases(core);
    decimal_cases(core);
    std::cout << "Verified " << checked << " native accumulator/BCD cases and " << unqualified
              << " explicit unqualified DAA state boundaries without retirement.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
