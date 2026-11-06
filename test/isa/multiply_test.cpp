// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "multiply_divide_model.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
using namespace cpu12::test::math;
constexpr std::array<unsigned, 14> anchors{
    0, 1, 2, 3, 7, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff};
constexpr std::array<unsigned, 4> flags{0, byte_mask, 0xa5, 0x5a};

void byte_domain(ObservedCore &core) {
  for (unsigned data = 0; data <= word_mask; ++data) {
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      auto before = seed;
      before.d = static_cast<std::uint16_t>(data);
      before.ccr = static_cast<std::uint8_t>(ccr);
      const Input point{.operation = Operation::mul, .before = before};
      retired(core, point, multiply(point));
    }
  }
}

void word_domain(ObservedCore &core, const Operation operation) {
  for (const auto anchor : anchors) {
    for (unsigned data = 0; data <= word_mask; ++data) {
      for (const auto ccr : flags) {
        auto before = seed;
        before.d = static_cast<std::uint16_t>(data);
        before.y = static_cast<std::uint16_t>(anchor);
        before.ccr = static_cast<std::uint8_t>(ccr);
        const Input point{.operation = operation, .before = before};
        retired(core, point, multiply(point));
      }
      auto before = seed;
      before.d = static_cast<std::uint16_t>(anchor);
      before.y = static_cast<std::uint16_t>(data);
      const Input point{.operation = operation, .before = before};
      retired(core, point, multiply(point));
    }
  }
}

void directed_flags(ObservedCore &core) {
  constexpr std::array<std::uint16_t, 4> pcs{0, 1, 0xfffe, 0xffff};
  constexpr std::array<Operation, 3> operations{Operation::mul, Operation::emul, Operation::emuls};
  for (const auto operation : operations) {
    for (const auto pc : pcs) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        for (const auto data : anchors) {
          auto before = seed;
          before.pc = pc;
          before.d = static_cast<std::uint16_t>(data);
          before.y = static_cast<std::uint16_t>(word_mask - data);
          before.ccr = static_cast<std::uint8_t>(ccr);
          const Input point{.operation = operation, .before = before};
          retired(core, point, multiply(point));
        }
      }
    }
  }
}

void literal_rounding(ObservedCore &core) {
  auto before = seed;
  constexpr std::uint16_t operands = 0x0fff;
  constexpr std::uint16_t product = 0x0ef1;
  before.d = operands;
  before.ccr = 0;
  core.set_registers(before);
  constexpr std::array<std::uint8_t, 1> multiply_code{0x12};
  require(core.run_instruction(multiply_code).retired == 1 && core.registers().d == product &&
              core.registers().ccr == 1,
          "Literal15 times255 has product0EF1 and carry from product bit7");
  constexpr std::array<std::uint8_t, 2> round{0x89, 0};
  constexpr std::uint16_t rounded = 0x0ff1;
  require(core.run_instruction(round).retired == 1 && core.registers().d == rounded,
          "Native ADCA immediate rounds the high product byte using MUL's real carry");
  checked += 2;
}

void refusals() {
  ObservedCore core;
  constexpr std::array<std::array<std::uint8_t, 3>, 3> malformed{
      {{0x12, 0, 0}, {0x18, 0x10, 0}, {0x18, 0x13, 0}}};
  for (const auto code : malformed) {
    core.set_registers(seed);
    require(core.run_instruction(code).fault == Fault::unsupported_instruction &&
                core.registers() == seed,
            "Padded arithmetic captures refuse without CPU mutation");
  }
  constexpr std::array<std::uint8_t, 1> prefix{0x18};
  require(core.run_instruction(prefix).fault == Fault::unsupported_instruction &&
              core.statistics().native_instructions == 0,
          "Truncated page-prefixed arithmetic cannot enter native code");
}
}

int main() {
  try {
    ObservedCore core;
    byte_domain(core);
    word_domain(core, Operation::emul);
    word_domain(core, Operation::emuls);
    directed_flags(core);
    literal_rounding(core);
    divisions(core);
    refusals();
    require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
                core.statistics().native_instructions == checked,
            "All multiply/divide operations use generated native arithmetic without memory or ISA "
            "helpers");
    std::cout << "Verified " << checked << " native multiply/divide cases; " << policy_cases
              << " include explicitly documented indeterminate-output policy, separate from "
                 "defined architectural checks.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
