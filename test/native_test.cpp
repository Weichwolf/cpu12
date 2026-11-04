// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
constexpr unsigned values = 256;
constexpr unsigned byte_mask = 0xff;
constexpr unsigned sign_bit = 0x80;
constexpr unsigned carry = 1;
constexpr unsigned overflow = 2;
constexpr unsigned zero = 4;
constexpr unsigned negative = 8;
constexpr unsigned half = 0x20;
constexpr unsigned nzv = negative | zero | overflow;
constexpr unsigned nzvc = nzv | carry;
constexpr unsigned arithmetic_flags = nzvc | half;
constexpr unsigned accumulator_mask = 0xbf;
constexpr unsigned other_a = 0xa500;
constexpr unsigned other_b = 0x5a;
constexpr unsigned high_mask = 0xff00;
constexpr cpu12::Registers seed{.pc = 0xfffe, .d = 0x3210, .x = 0x1234, .y = 0x5678, .sp = 0x2000};

enum OracleOpcode : std::uint8_t {
  load_a = 0x86,
  add_a = 0x8b,
  sub_a = 0x80,
  cmp_a = 0x81,
  and_a = 0x84,
  or_a = 0x8a,
  xor_a = 0x88,
  test_a = 0x85
};

unsigned cases = 0;

void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

int signed_byte(const unsigned value) {
  return value < sign_bit ? static_cast<int>(value)
                          : static_cast<int>(value) - static_cast<int>(values);
}

struct Input {
  unsigned opcode, left, right, ccr;
};

struct Expected {
  unsigned value, ccr;
  bool store = true;
};

Expected expected(const Input input) {
  const auto opcode = input.opcode & accumulator_mask;
  const auto left = input.left;
  const auto right = input.right;
  auto value = right;
  unsigned changed = nzv;
  unsigned flags = 0;
  bool store = true;
  switch (opcode) {
    case load_a: break;
    case add_a: {
      const auto sum = left + right;
      value = sum & byte_mask;
      const auto signed_sum = signed_byte(left) + signed_byte(right);
      flags = (sum >= values ? carry : 0U) |
              (signed_sum < -static_cast<int>(sign_bit) || signed_sum >= static_cast<int>(sign_bit)
                   ? overflow
                   : 0U) |
              ((left % 16 + right % 16) >= 16 ? half : 0U);
      changed = arithmetic_flags;
      break;
    }
    case sub_a:
    case cmp_a: {
      const auto difference = static_cast<int>(left) - static_cast<int>(right);
      value = static_cast<unsigned>(difference + static_cast<int>(values)) % values;
      const auto signed_difference = signed_byte(left) - signed_byte(right);
      flags =
          (difference < 0 ? carry : 0U) | (signed_difference < -static_cast<int>(sign_bit) ||
                                                   signed_difference >= static_cast<int>(sign_bit)
                                               ? overflow
                                               : 0U);
      changed = nzvc;
      store = opcode != cmp_a;
      break;
    }
    case and_a: value = left & right; break;
    case or_a: value = left | right; break;
    case xor_a: value = left ^ right; break;
    case test_a:
      value = left & right;
      store = false;
      break;
    default: throw std::runtime_error("Unsupported test oracle operation");
  }
  flags |= (value == 0 ? zero : 0U) | (value >= sign_bit ? negative : 0U);
  return {.value = value, .ccr = (input.ccr & (byte_mask ^ changed)) | flags, .store = store};
}

void arithmetic_case(cpu12::Core &core, const Input input) {
  const bool b = (input.opcode & 0x40U) != 0;
  const auto d = b ? other_a | input.left : (input.left * values) | other_b;
  auto before = seed;
  before.d = static_cast<std::uint16_t>(d);
  before.ccr = static_cast<std::uint8_t>(input.ccr);
  auto wanted = before;
  const auto oracle = expected(input);
  wanted.pc = 0;
  wanted.ccr = static_cast<std::uint8_t>(oracle.ccr);
  if (oracle.store) {
    wanted.d = static_cast<std::uint16_t>(b ? (d & high_mask) | oracle.value
                                            : (oracle.value * values) | (d & byte_mask));
  }
  core.set_registers(before);
  const std::array<std::uint8_t, 2> instruction{static_cast<std::uint8_t>(input.opcode),
                                                static_cast<std::uint8_t>(input.right)};
  const auto result = core.execute(instruction);
  require(result.retired == 1 && result.fault == cpu12::Fault::none && result.pc == before.pc &&
              core.registers() == wanted,
          "Native immediate arithmetic/CCR/register expectation failed");
  ++cases;
}

void arithmetic(cpu12::Core &core) {
  constexpr std::array<unsigned, 16> opcodes{0x86,
                                             0xc6,
                                             0x8b,
                                             0xcb,
                                             0x80,
                                             0xc0,
                                             0x81,
                                             0xc1,
                                             0x84,
                                             0xc4,
                                             0x8a,
                                             0xca,
                                             0x88,
                                             0xc8,
                                             0x85,
                                             0xc5};
  constexpr std::array<unsigned, 3> old_flags{0, 0xd0, 0xff};
  for (const auto opcode : opcodes) {
    for (unsigned right = 0; right < values; ++right) {
      for (const auto ccr : old_flags) {
        for (unsigned left = 0; left < values; ++left) {
          arithmetic_case(core, {.opcode = opcode, .left = left, .right = right, .ccr = ccr});
        }
      }
    }
  }
}

struct BranchInput {
  unsigned condition, ccr;
};

bool take_branch(const BranchInput input) {
  const auto condition = input.condition;
  const auto ccr = input.ccr;
  const bool c = (ccr & carry) != 0;
  const bool z = (ccr & zero) != 0;
  const bool n = (ccr & negative) != 0;
  const bool v = (ccr & overflow) != 0;
  switch (condition) {
    case 0: return true;
    case 1: return false;
    case 2: return !c && !z;
    case 3: return c || z;
    case 4: return !c;
    case 5: return c;
    case 6: return !z;
    case 7: return z;
    case 8: return !v;
    case 9: return v;
    case 10: return !n;
    case 11: return n;
    case 12: return n == v;
    case 13: return n != v;
    case 14: return !z && n == v;
    case 15: return z || n != v;
    default: throw std::runtime_error("Unsupported branch oracle");
  }
}

void branches(cpu12::Core &core) {
  constexpr std::array<int, 5> displacements{-128, -1, 0, 1, 127};
  for (unsigned condition = 0; condition < 16; ++condition) {
    for (const auto displacement : displacements) {
      const std::array<std::uint8_t, 2> instruction{static_cast<std::uint8_t>(0x20U + condition),
                                                    static_cast<std::uint8_t>(displacement)};
      for (unsigned ccr = 0; ccr < values; ++ccr) {
        auto before = seed;
        before.ccr = static_cast<std::uint8_t>(ccr);
        auto wanted = before;
        wanted.pc = static_cast<std::uint16_t>(
            take_branch({.condition = condition, .ccr = ccr}) ? displacement : 0);
        core.set_registers(before);
        const auto result = core.execute(instruction);
        require(result.retired == 1 && result.fault == cpu12::Fault::none &&
                    core.registers() == wanted,
                "Independent signed/unsigned branch and PC-wrap expectation failed");
        ++cases;
      }
    }
  }
}

void refusal(cpu12::Core &core) {
  const auto state = core.registers();
  const auto count = core.statistics().native_instructions;
  constexpr std::array<std::uint8_t, 1> unsupported{0};
  constexpr std::array<std::uint8_t, 1> truncated{0x86};
  for (const auto &instruction : {unsupported, truncated}) {
    const auto result = core.execute(instruction);
    require(result.fault == cpu12::Fault::unsupported_instruction && result.retired == 0 &&
                core.registers() == state,
            "Unsupported instructions refuse visibly before any architectural change");
  }
  require(core.execute({}).fault == cpu12::Fault::invalid_input &&
              core.statistics().native_instructions == count,
          "Invalid input and refusals retire no native instruction");
  core.clear_code_cache();
  constexpr std::array<std::uint8_t, 1> nop{0xa7};
  auto wanted = state;
  wanted.pc = static_cast<std::uint16_t>(wanted.pc + 1U);
  const auto result = core.execute(nop);
  require(result.retired == 1 && core.registers() == wanted,
          "Cache clear and NOP retain all non-PC registers");
  ++cases;
}
}

int main() {
  try {
    require(cpu12::Core::native_supported(),
            "Native x86-64/Linux execution is required by this directed test");
    cpu12::Core core;
    arithmetic(core);
    branches(core);
    refusal(core);
    require(core.statistics().native_instructions == cases &&
                core.statistics().compiled_entries > 0,
            "Every asserted retirement actually entered generated native code");
    std::cout << "Verified " << cases
              << " independent native arithmetic/branch cases; unsupported ISA remains explicit.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
