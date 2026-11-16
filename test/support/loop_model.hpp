// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "isa.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>

namespace cpu12::test {
// Independently authored fixtures alternate one operation and its backward BRA.
struct Loop {
  Address entry;
  Byte opcode;
  unsigned length, cycles;
};

struct LoopPrefix {
  State state;
  RunResult result;
  std::uint64_t cycles = 0;
};

inline LoopPrefix loop_prefix(const Registers seed,
                              const Loop loop,
                              const std::uint64_t bound,
                              const bool deadline,
                              const auto &operation) {
  LoopPrefix expected{.state = {.registers = seed, .failure = {}}, .result = {.pc = seed.pc}};
  while ((deadline ? expected.cycles : expected.result.retired) < bound) {
    const auto before = expected.state.registers.pc;
    const bool branch = before == static_cast<Address>(loop.entry + loop.length);
    const auto retired = expected.result.retired;
    if (branch) {
      expected.state.registers.pc = loop.entry;
      expected.state.irq_delay = 0;
    } else {
      expected.state = operation(expected.state.registers);
      if (expected.state.failure.fault != Fault::none) {
        expected.result = expected.state.failure;
        expected.result.retired = retired;
        break;
      }
    }
    expected.result = {.retired = retired + 1,
                       .pc = before,
                       .opcode =
                           branch ? static_cast<Byte>(isa::Opcode::bra_relative) : loop.opcode};
    expected.cycles += branch ? 3 : loop.cycles;
  }
  return expected;
}

inline void verify_loop(Core &core, const Registers seed, const Loop loop, const auto &operation) {
  constexpr std::array<std::uint32_t, 8> bounds{0, 1, 2, 3, 4, 5, 16, 17};
  for (const auto pc : {seed.pc, static_cast<Address>(loop.entry + loop.length)}) {
    auto initial = seed;
    initial.pc = pc;
    for (const bool deadline : {false, true}) {
      for (const auto bound : bounds) {
        core.set_registers(initial);
        const auto before = core.progress().estimated_cycles;
        const auto expected = loop_prefix(initial, loop, bound, deadline, operation);
        const auto result = deadline ? core.run_until(before + bound) : core.run(bound);
        if (result != expected.result || core.state() != expected.state ||
            core.progress().estimated_cycles != before + expected.cycles) {
          throw std::runtime_error("Native loop disagrees with the independent ISA prefix model");
        }
      }
    }
  }
}
}
