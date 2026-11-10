// SPDX-License-Identifier: GPL-3.0-only
#include "accumulator_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "decimal_model.hpp"
#include "isa.hpp"
#include "preparation_registers.hpp"

#include <array>
#include <cstddef>

namespace cpu12::test::prepared_registers {
namespace {
std::size_t binary() {
  constexpr std::array codes{
      code<isa::Opcode::aba>(), code<isa::Opcode::sba>(), code<isa::Opcode::cba>()};
  constexpr std::array operations{accumulator::Operation::add,
                                  accumulator::Operation::subtract,
                                  accumulator::Operation::compare};
  constexpr std::array<Byte, 8> flags{0, 1, 2, 4, 8, 0x20, 0xd0, 0xff};
  Pair pair(codes);
  std::size_t count = 0;
  for (std::size_t index = 0; index < codes.size(); ++index) {
    for (unsigned a = 0; a <= byte_mask; ++a) {
      for (unsigned b = 0; b <= byte_mask; ++b) {
        for (const auto ccr : flags) {
          auto seed = base;
          seed.pc = pair.program.entries[index];
          seed.d = static_cast<Address>((a << byte_bits) | b);
          seed.ccr = ccr;
          const auto wanted = accumulator::arithmetic(
              {.operation = operations[index], .before = seed}, {.left = a, .right = b});
          check(pair, seed, codes[index], {.registers = wanted, .failure = {}}, 2);
          ++count;
        }
      }
    }
    constexpr std::array<Address, 7> edges{0, 1, 0x007f, 0x7f01, 0x8001, 0x80ff, 0xffff};
    for (const auto value : edges) {
      auto seed = base;
      seed.pc = pair.program.entries[index];
      seed.d = value;
      seed.ccr = byte_mask;
      prefixes(pair, seed);
    }
  }
  pair.unchanged();
  return count;
}

std::size_t decimal() {
  constexpr auto encoding = code<isa::Opcode::daa>();
  constexpr std::array codes{encoding};
  Pair pair(codes);
  std::size_t count = 0;
  for (unsigned a = 0; a <= byte_mask; ++a) {
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      auto seed = base;
      seed.pc = entry;
      seed.d = static_cast<Address>((a << byte_bits) | (seed.d & byte_mask));
      seed.ccr = static_cast<Byte>(ccr);
      const auto rule = accumulator::rule(seed);
      State wanted{.registers = seed, .failure = {}};
      if (rule) {
        wanted.registers = accumulator::decimal_result(
            seed, {.a = (a + rule->correction) % (byte_mask + 1U), .carry = rule->result_carry});
      } else {
        wanted.failure = {.fault = Fault::unsupported_state,
                          .pc = seed.pc,
                          .opcode = encoding.bytes[0],
                          .value = static_cast<Address>(accumulator::actual_input(seed))};
      }
      check(pair, seed, encoding, wanted, rule ? 3 : 0);
      ++count;
      if (ccr == 0 || ccr == byte_mask) { prefixes(pair, seed); }
    }
  }
  pair.unchanged();
  auto isolated = encoding.bytes;
  Core single(Memory::window({.first = entry, .bytes = isolated, .writable = false}));
  constexpr std::array entries{entry};
  const auto report = single.prepare(entries);
  require(report.prepared_entries == 1 && report.native_bytes > 0 &&
              report.failure.fault == Fault::none,
          "An isolated decimal instruction also uses the complete LLVM pipeline");
  constexpr Registers invalid{.pc = entry, .d = 0x345a, .ccr = 0x20};
  single.set_registers(invalid);
  const auto failure = single.run(1);
  require(failure.fault == Fault::unsupported_state && failure.retired == 0 &&
              failure.value == accumulator::actual_input(invalid) &&
              single.registers() == invalid && single.progress() == Progress{},
          "Isolated invalid decimal input reports original A/C/H without retirement");
  return count;
}
}

std::size_t accumulator_cases() {
  return binary() + decimal();
}
}
