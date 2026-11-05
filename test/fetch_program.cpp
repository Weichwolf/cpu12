// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fetch_model.hpp"

#include <algorithm>
#include <array>

namespace cpu12::test::fetching {
namespace {
constexpr Address start = 0x200;
constexpr Registers seed{.pc = start, .d = 0x55aa, .y = 0xa55a, .sp = 0x9000, .ccr = 0xa1};
using Storage = std::array<Byte, address_bytes>;

void loop() {
  constexpr std::array<Byte, 14> program{
      0xce, 0x10, 0, 0xc6, 0x10, 0x86, 0, 0x8b, 3, 0x6a, 0x30, 4, 0x31, 0xf9};
  constexpr std::array<unsigned, 6> budgets{1, 2, 3, 7, 25, 50};
  constexpr Address output = 0x1000;
  constexpr Registers expected{.pc = start + program.size(),
                               .d = 0x3000,
                               .x = output + 16,
                               .y = seed.y,
                               .sp = seed.sp,
                               .ccr = 0xa0};
  for (const auto budget : budgets) {
    Storage storage{};
    std::ranges::copy(program, storage.begin() + start);
    Core core(Memory::unified(storage));
    core.set_registers(seed);
    const auto prefix = core.run(budget);
    require(prefix.retired == budget && prefix.fault == Fault::none,
            "Instruction budget stops a real native loop at the requested boundary");
    constexpr unsigned instructions = 51;
    const auto suffix = core.run(instructions - budget);
    require(suffix.retired == instructions - budget && suffix.fault == Fault::none &&
                core.registers() == expected &&
                core.statistics().native_instructions == instructions &&
                core.statistics().fetch_callbacks == 0 && core.statistics().read_callbacks == 0 &&
                core.statistics().write_callbacks == 0,
            "Fetched loop completes16 additions/stores with independent complete-register "
            "expectations");
    for (unsigned index = 0; index < 16; ++index) {
      require(storage[output + index] == 3 * (index + 1),
              "Native auto-indexed loop stores the independent arithmetic progression");
    }
    const auto terminal = core.run(1);
    require(terminal.fault == Fault::unsupported_instruction && terminal.retired == 0 &&
                terminal.pc == expected.pc && core.registers() == expected,
            "No invented STOP success is used after the authored program");
    native += instructions;
  }
}

void partial_budget_failure() {
  Storage storage{};
  constexpr std::array<Byte, 6> program{0x86, 0x7f, 0x8b, 1, 0xa7, 0};
  std::ranges::copy(program, storage.begin() + start);
  Core core(Memory::unified(storage));
  core.set_registers(seed);
  const auto result = core.run(10);
  constexpr Registers expected{
      .pc = start + 5, .d = 0x80aa, .y = seed.y, .sp = seed.sp, .ccr = 0xaa};
  require(result.retired == 3 && result.fault == Fault::unsupported_instruction &&
              result.pc == expected.pc && core.registers() == expected,
          "A bounded run reports only completed instructions before the precise terminal fault");
  require(core.run(10).retired == 0 && core.registers() == expected &&
              core.statistics().native_instructions == 3,
          "A later run does not recount completed instructions or replay a latched failure");
  native += 3;
}

void calls() {
  Storage storage{};
  constexpr std::array<Byte, 7> caller{0xcf, 0x80, 0, 0x16, 2, 0x20, 0xa7};
  constexpr std::array<Byte, 3> callee{0x86, 0x7d, 0x3d};
  constexpr Address target = 0x220;
  std::ranges::copy(caller, storage.begin() + start);
  std::ranges::copy(callee, storage.begin() + target);
  Core core(Memory::unified(storage));
  core.set_registers(seed);
  constexpr Registers expected{
      .pc = start + caller.size(), .d = 0x7daa, .y = seed.y, .sp = 0x8000, .ccr = 0xa1};
  constexpr Address saved = 0x7ffe;
  require(core.run(5).retired == 5 && core.registers() == expected && storage[saved] == 2 &&
              storage[saved + 1] == 6,
          "Fetched JSR/RTS uses the real saved next PC and restores the stack before caller NOP");
  native += 5;
}

void changed_code() {
  Storage storage{};
  constexpr std::array<Byte, 7> program{0x86, 0x86, 0x7a, 2, 5, 0xa7, 0x35};
  std::ranges::copy(program, storage.begin() + start);
  Core core(Memory::unified(storage));
  auto before = seed;
  before.pc = start + 5;
  core.set_registers(before);
  require(core.step().retired == 1, "Prewarm a one-byte NOP at the future modified address");
  core.set_registers(seed);
  const auto result = core.run(3);
  constexpr Registers expected{
      .pc = start + program.size(), .d = 0x35aa, .y = seed.y, .sp = seed.sp, .ccr = 0xa1};
  constexpr Byte load_a = 0x86;
  require(result.retired == 3 && result.fault == Fault::none && core.registers() == expected &&
              storage[start + 5] == load_a && core.statistics().compiled_entries == 4,
          "A guest store replaces cached NOP with actual two-byte LDAA without stale fetch "
          "length/code");
  native += 4;
  constexpr Byte changed_immediate = 0x55;
  storage[start + 6] = changed_immediate;
  before.pc = start + 5;
  core.set_registers(before);
  require(core.step().retired == 1 && core.registers().d == seed.d &&
              core.statistics().compiled_entries == 5,
          "An externally changed immediate byte also participates in the fetched native cache key");
  ++native;
}

struct Bank {
  static constexpr std::array<Byte, 7> first{0x86, 1, 0x7a, 0x12, 0x34, 0x86, 0x22};
  static constexpr std::array<Byte, 7> second{0x86, 1, 0x7a, 0x12, 0x34, 0x86, 0x55};
  bool selected = false;
  unsigned fetches = 0, writes = 0;

  static bool fetch(void *owner, const Address address, Byte &value) noexcept {
    auto &bank = *static_cast<Bank *>(owner);
    ++bank.fetches;
    if (address < start || static_cast<unsigned>(address - start) >= first.size()) { return false; }
    value = (bank.selected ? second : first)[static_cast<unsigned>(address - start)];
    return true;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &bank = *static_cast<Bank *>(owner);
    ++bank.writes;
    constexpr Address select = 0x1234;
    if (access.address != select || access.value != 1) { return false; }
    bank.selected = true;
    return true;
  }
};

void bank_change() {
  Bank bank;
  Io io{.owner = &bank, .write = Bank::write, .fetch = Bank::fetch};
  Core core(Memory::external(), &io);
  auto before = seed;
  before.pc = start + 5;
  core.set_registers(before);
  require(core.step().retired == 1, "Prewarm future code from the first external bank");
  core.set_registers(seed);
  constexpr Registers expected{
      .pc = start + Bank::first.size(), .d = 0x55aa, .y = seed.y, .sp = seed.sp, .ccr = 0xa1};
  const auto result = core.run(3);
  require(
      result.retired == 3 && result.fault == Fault::none && core.registers() == expected &&
          bank.selected && bank.fetches == 9 && bank.writes == 1 &&
          core.statistics().compiled_entries == 4 && core.statistics().fetch_callbacks == 9 &&
          core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 1,
      "Guest bank-select write changes the next fetched operand despite a warm prior-bank entry");
  native += 4;
}
}

void program_cases() {
  loop();
  partial_budget_failure();
  calls();
  changed_code();
  bank_change();
}
}
