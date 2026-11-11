// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "native_flow.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>

namespace cpu12::test::native_flow {
void Model::step() {
  last_pc = registers.pc;
  last_opcode = bytes[last_pc];
  const auto push = [&](const Address destination, const Address next) {
    registers.sp = static_cast<Address>(unsigned{registers.sp} + address_bytes - 2);
    bytes[registers.sp] = static_cast<Byte>(next >> byte_bits);
    bytes[static_cast<Address>(registers.sp + 1U)] = static_cast<Byte>(next);
    registers.pc = destination;
    cycles += 4;
  };
  const auto pull = [&] {
    registers.pc = static_cast<Address>((unsigned{bytes[registers.sp]} << byte_bits) |
                                        bytes[static_cast<Address>(registers.sp + 1U)]);
    registers.sp = static_cast<Address>(registers.sp + 2U);
    cycles += 5;
  };
  switch (registers.pc) {
    case entry: push(first, entry + 3); break;
    case entry + 3: push(second, entry + 6); break;
    case entry + 6:
      registers.pc = entry;
      cycles += 3;
      break;
    case first:
      registers.x = x_value;
      registers.ccr &= preserved;
      registers.pc += 3;
      cycles += 2;
      break;
    case first + 3: push(nested, first + 6); break;
    case first + 6:
    case second + 2:
    case nested + 2: pull(); break;
    case second:
      registers.d =
          static_cast<Address>((unsigned{a_value} << byte_bits) | (registers.d & byte_mask));
      registers.ccr &= preserved;
      registers.pc += 2;
      ++cycles;
      break;
    case nested:
      registers.d = static_cast<Address>((registers.d & high_mask) | b_value);
      registers.ccr = static_cast<Byte>((registers.ccr & preserved) | 8U);
      registers.pc += 2;
      ++cycles;
      break;
    default: require(false, "The independent flow model reached an unauthored instruction");
  }
  ++retired;
}

namespace {
void check(const Core &core,
           const Fixture &fixture,
           const Model &model,
           const RunResult result,
           const std::uint64_t before) {
  require(result.retired == model.retired && result.fault == Fault::none &&
              core.state() == State{.registers = model.registers, .failure = {}} &&
              core.progress().estimated_cycles == before + model.cycles &&
              fixture.bytes == model.bytes,
          "Native unit transfers preserve complete independently modeled state, actual stack RAM "
          "and cycles");
  if (model.retired != 0) {
    require(
        result.pc == model.last_pc && result.opcode == model.last_opcode,
        "Zero-progress successor guards retain the actual last completed PC/opcode across units");
  }
}

std::size_t prefixes(Core &core, Fixture &fixture) {
  const auto original = fixture.bytes;
  const auto compiled = core.statistics().compiled_entries;
  constexpr std::array<Address, 4> positions{0, 1, 4, seed.sp};
  constexpr std::array<Byte, 4> flags{0, 0x55, 0xaa, 0xff};
  std::size_t count = 0;
  for (const auto sp : positions) {
    for (const auto ccr : flags) {
      for (unsigned budget = 0; budget <= 64; ++budget) {
        auto before = seed;
        before.sp = sp;
        before.ccr = ccr;
        core.set_registers(before);
        fixture.bytes = original;
        Model model{.registers = before, .bytes = original};
        for (unsigned step = 0; step < budget; ++step) { model.step(); }
        const auto cycles = core.progress().estimated_cycles;
        check(core, fixture, model, core.run(budget), cycles);
        require(core.statistics().compiled_entries == compiled,
                "Every prepared direct/indirect/return transition executes without translation");
        ++count;
      }
    }
  }
  return count;
}

void deadlines(Core &core, Fixture &fixture) {
  const auto original = fixture.bytes;
  const auto compiled = core.statistics().compiled_entries;
  constexpr unsigned bound = 128;
  for (unsigned delta = 0; delta <= bound; ++delta) {
    fixture.bytes = original;
    core.set_registers(seed);
    Model model{.registers = seed, .bytes = original};
    while (model.cycles < delta) { model.step(); }
    const auto cycles = core.progress().estimated_cycles;
    check(core, fixture, model, core.run_until(cycles + delta), cycles);
    require(core.statistics().compiled_entries == compiled,
            "Cross-unit deadlines never compile, re-fetch or replay a native prefix");
  }
}

void missing(Core &core, Fixture &fixture) {
  constexpr std::array<Address, 2> targets{0x4110, 0x5000};
  for (const auto pc : targets) {
    auto before = seed;
    before.pc = nested + 2;
    fixture.bytes[before.sp] = static_cast<Byte>(pc >> byte_bits);
    fixture.bytes[before.sp + 1] = static_cast<Byte>(pc);
    core.set_registers(before);
    const auto cycles = core.progress().estimated_cycles;
    const auto compiled = core.statistics().compiled_entries;
    auto wanted = before;
    wanted.pc = pc;
    wanted.sp += 2;
    const auto returned = core.run(1);
    require(
        returned.retired == 1 && returned.fault == Fault::none && returned.pc == before.pc &&
            returned.opcode == nested_code.back() && core.registers() == wanted &&
            core.progress().estimated_cycles == cycles + 5,
        "Missing native table entries/pages preserve the completed original return instruction");
    const auto refused = core.run(1);
    require(refused.retired == 0 && refused.fault == Fault::unprepared_execution &&
                refused.pc == pc && refused.address == pc && core.registers() == wanted &&
                core.progress().estimated_cycles == cycles + 5 &&
                core.statistics().compiled_entries == compiled,
            "A strict missing prepared target faults before any decode, first-use emission or time "
            "advance");
  }
}

void long_run(Core &core, Fixture &fixture) {
  constexpr std::uint32_t budget = 1'000'000;
  core.set_registers(seed);
  Model model{.registers = seed, .bytes = fixture.bytes};
  for (std::uint32_t step = 0; step < budget; ++step) { model.step(); }
  const auto cycles = core.progress().estimated_cycles;
  check(core, fixture, model, core.run(budget), cycles);
}
}
}

int main() {
  try {
    using namespace cpu12::test::native_flow;
    const auto image = compile();
    Fixture fixture;
    cpu12::Io io{.owner = &fixture, .read_word = Fixture::read, .write_word = Fixture::write};
    cpu12::Core core(fixture.memory(), &io);
    core.install(image, variant_id);
    const auto count = prefixes(core, fixture);
    deadlines(core, fixture);
    missing(core, fixture);
    long_run(core, fixture);
    reuse();
    std::cout << "Verified " << count
              << " independent native-unit prefixes, deadlines, missing targets, code dependencies "
                 "and bounded host stack.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
