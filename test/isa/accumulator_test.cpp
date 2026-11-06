// SPDX-License-Identifier: GPL-3.0-only
#include "accumulator_model.hpp"
#include "cpu12/core.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
using namespace cpu12::test::accumulator;

void full_domain(ObservedCore &core, const Operation operation) {
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

void pc_wrap(ObservedCore &core) {
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

void aliases(ObservedCore &core) {
  constexpr std::array<std::uint16_t, 8> bases{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xfffe, 0xffff};
  constexpr std::array<unsigned, 4> flags{0, 0xff, 0xa5, 0x5a};
  constexpr std::array<std::array<std::uint8_t, 2>, 2> instructions{{{0x1a, 0xe5}, {0x19, 0xed}}};
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

void refusals() {
  ObservedCore core;
  constexpr std::array<std::array<std::uint8_t, 3>, 4> padded{
      {{0x18, 6, 0}, {0x18, 7, 0}, {0x18, 0x16, 0}, {0x18, 0x17, 0}}};
  for (const auto code : padded) {
    core.set_registers(seed);
    require(core.run_instruction(code).fault == Fault::unsupported_instruction &&
                core.registers() == seed,
            "Padded accumulator captures refuse before native retirement");
  }
  constexpr std::array<std::uint8_t, 1> prefix{0x18};
  require(core.run_instruction(prefix).fault == Fault::unsupported_instruction &&
              core.statistics().compiled_entries == 0,
          "Truncated accumulator prefix does not compile");
}
}

int main() {
  try {
    ObservedCore core;
    full_domain(core, Operation::add);
    full_domain(core, Operation::subtract);
    full_domain(core, Operation::compare);
    pc_wrap(core);
    aliases(core);
    decimal_cases(core);
    refusals();
    require(core.statistics().native_instructions == checked &&
                core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
            "Accumulator/BCD instructions and existing aliases execute natively without data "
            "callbacks");
    std::cout << "Verified " << checked << " native accumulator/BCD cases and " << unqualified
              << " explicit unqualified DAA state boundaries without retirement.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
