// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "mapped_memory.hpp"

#include <array>
#include <exception>
#include <iostream>

namespace {
using namespace cpu12;
using namespace cpu12::test::mapped;

constexpr Byte accumulator_a = 0x12, accumulator_b = 0x34;
constexpr Byte replacement = 0xa5, known_flag = 0x80;
constexpr Byte maximum = 0xff, positive_ccr = 0xf1, increment_ccr = 0xf5;
constexpr Byte indirect_value = 0xab;
constexpr Address replaced_d = 0xa534, indirect_d = 0xab34;

void byte_access() {
  Storage bytes;
  auto grants = bytes.grants();
  Owner owner;
  auto io = owner.io();
  Core core(Memory::mapped(grants), &io);
  constexpr auto store = extended<isa::Opcode::staa_extended>(second + 3);
  constexpr auto load = extended<isa::Opcode::ldaa_extended>(second + 3);
  core.set_registers(seed);
  std::array<Byte, 16> expected{};
  std::array<Byte, 16> known{};
  expected[3] = accumulator_a;
  known[3] = 1;
  require(core.run_instruction(store).retired == 1 && bytes.high == expected &&
              bytes.high_known == known && bytes.low == std::array<Byte, 16>{} &&
              bytes.low_known == std::array<Byte, 16>{} && owner.writes == 0,
          "A native store in the second grant initializes exactly its real byte");
  bytes.high[3] = replacement;
  bytes.high_known[3] = known_flag;
  core.set_registers(seed);
  require(core.run_instruction(load).retired == 1 && core.registers().d == replaced_d &&
              owner.reads == 0,
          "Native reads accept any nonzero live knownness and see current storage");
  const auto compiled = core.statistics().compiled_entries;
  bytes.high_known[3] = 0;
  core.set_registers(seed);
  const auto failed = core.run_instruction(load);
  require(failed.fault == Fault::memory_read && failed.address == second + 3 &&
              failed.access_size == 1 && core.registers() == seed && owner.reads == 1 &&
              core.statistics().compiled_entries == compiled,
          "Previously translated code observes new unknownness and reports the actual owner fault");
}

void words() {
  Storage bytes;
  const auto grants = bytes.grants();
  Owner owner;
  auto io = owner.io();
  Core core(Memory::mapped(grants), &io);
  constexpr auto store = extended<isa::Opcode::std_extended>(first + 1);
  constexpr auto load = extended<isa::Opcode::ldd_extended>(first + 1);
  core.set_registers(seed);
  require(core.run_instruction(store).retired == 1 && bytes.low[1] == accumulator_a &&
              bytes.low[2] == accumulator_b && bytes.low_known[1] == 1 && bytes.low_known[2] == 1 &&
              owner.writes == 0 && core.registers().ccr == positive_ccr,
          "An odd native word store preserves big-endian values, CCR and both known bytes");
  bytes.low_known[2] = 0;
  core.set_registers(seed);
  const auto refused = core.run_instruction(load);
  require(refused.fault == Fault::memory_read && refused.access_size == 2 &&
              refused.address == first + 1 && owner.reads == 1 && owner.width == 2 &&
              core.registers() == seed,
          "A partially unknown word uses one real owner word operation before any CPU publication");
  bytes.low_known[2] = 2;
  core.set_registers(seed);
  require(core.run_instruction(load).retired == 1 && core.registers().d == seed.d &&
              owner.reads == 1,
          "The unchanged native word entry sees newly initialized low-byte knowledge");
}

void modifying_and_indirect() {
  Storage bytes;
  const auto grants = bytes.grants();
  Core core(Memory::mapped(grants));
  bytes.low[4] = maximum;
  bytes.low_known[4] = known_flag;
  core.set_registers(seed);
  constexpr auto increment = extended<isa::Opcode::inc_extended>(first + 4);
  require(core.run_instruction(increment).retired == 1 && bytes.low[4] == 0 &&
              bytes.low_known[4] == 1 && core.registers().ccr == increment_ccr,
          "Native mapped read-modify-write retains result flags and canonical knownness");
  constexpr auto clear = extended<isa::Opcode::clr_extended>(second + 7);
  core.set_registers(seed);
  require(core.run_instruction(clear).retired == 1 && bytes.high[7] == 0 &&
              bytes.high_known[7] == 1,
          "Write-only CLR initializes an unknown byte without a fabricated read");
  bytes.low[0] = static_cast<Byte>(second >> 8U);
  bytes.low[1] = 3;
  bytes.low_known[0] = 1;
  bytes.low_known[1] = 1;
  bytes.high[3] = indirect_value;
  bytes.high_known[3] = 1;
  constexpr auto indirect =
      isa::program(isa::opcode<isa::Opcode::ldaa_indirect_16>(), std::array<Byte, 3>{0xe3, 0, 0});
  core.set_registers(seed);
  require(core.run_instruction(indirect).retired == 1 && core.registers().d == indirect_d,
          "An indirect pointer and its target may reside in different native RAM grants");
}
}

int main() {
  try {
    byte_access();
    words();
    modifying_and_indirect();
    cpu12::test::mapped::mapping_cases();
    std::cout
        << "Mapped RAM, live knownness, native stores, aliases and retained remapping passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
