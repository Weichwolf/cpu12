// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "masked_read_model.hpp"
#include "observed_core.hpp"

#include <array>

namespace cpu12::test::masked {
namespace {
constexpr std::array<Byte, 5> branch{0x1e, 0xab, 0xcd, 0x0f, 0};
constexpr Address port = 0xabcd;
constexpr Byte mixed_input = 0xa5;
constexpr Byte fallback_input = 0x85;

void refused() {
  Bus bus{.value = mixed_input, .reject_selected = true};
  Io io{.owner = &bus, .read = Bus::read, .read_masked = Bus::masked};
  ObservedCore core(Memory::external(), &io);
  core.set_registers(seed);
  const auto result = core.run_instruction(branch);
  require(result.retired == 0 && result.fault == Fault::memory_read && result.pc == seed.pc &&
              result.address == port && result.value == mixed_input && result.access_size == 1 &&
              result.read_mask == branch[3] && core.registers() == seed && bus.selected == 1 &&
              bus.full == 0 && bus.value == 0,
          "Refused selected reads retain the actual diagnostic value and never retry a full read");
  require(core.run_instruction(branch).fault == Fault::memory_read && bus.selected == 1 &&
              bus.full == 0,
          "Masked-read failures latch without repeating destructive side effects");
  core.set_registers(seed);
  bus.allowed = false;
  constexpr std::array<Byte, 3> full_read{0xb6, 0xab, 0xcd};
  const auto ordinary = core.run_instruction(full_read);
  constexpr unsigned all = 0xff;
  require(ordinary.fault == Fault::memory_read && ordinary.read_mask == all && bus.full == 1 &&
              bus.selected == 1 && core.registers() == seed,
          "A later ordinary read resets mask diagnostics and uses its own full-byte callback");
  auto decimal = seed;
  constexpr Address invalid_decimal = 0x955a;
  constexpr Byte half = 0x20;
  decimal.d = invalid_decimal;
  decimal.ccr = half;
  core.set_registers(decimal);
  constexpr std::array<Byte, 2> daa{0x18, 7};
  const auto unqualified = core.run_instruction(daa);
  require(unqualified.fault == Fault::unsupported_state && unqualified.read_mask == 0 &&
              unqualified.access_size == 0 && unqualified.address == 0 &&
              core.registers() == decimal,
          "An explicit boundary prevents old selected-bit metadata leaking into a nonmemory fault");
}

void fault_masks() {
  Bus bus{.reject_selected = true};
  Io io{.owner = &bus, .read = Bus::read, .read_masked = Bus::masked};
  ObservedCore core(Memory::external(), &io);
  constexpr unsigned maximum = 0xff;
  constexpr Byte rejected_value = 0xa5;
  for (unsigned mask = 0; mask <= maximum; ++mask) {
    auto code = branch;
    code[3] = static_cast<Byte>(mask);
    bus.value = rejected_value;
    core.set_registers(seed);
    const auto result = core.run_instruction(code);
    require(
        result.retired == 0 && result.fault == Fault::memory_read && result.pc == seed.pc &&
            result.address == port && result.value == rejected_value && result.read_mask == mask &&
            result.access_size == 1 && core.registers() == seed && bus.full == 0 &&
            bus.selected == mask + 1 && bus.value == 0,
        "Every rejected mask, including00/ff, retains exact diagnostics without a full-read retry");
  }
}

void ordinary_fallback() {
  Bus bus{.value = fallback_input};
  Io io{.owner = &bus, .read = Bus::read};
  ObservedCore core(Memory::external(), &io);
  core.set_registers(seed);
  auto wanted = seed;
  wanted.pc = static_cast<Address>(seed.pc + branch.size());
  const auto result = core.run_instruction(branch);
  require(result.retired == 1 && result.fault == Fault::none && core.registers() == wanted &&
              bus.full == 1 && bus.selected == 0 && bus.value == 0 &&
              core.statistics().read_callbacks == 1,
          "Absent selected interface uses one actual qualified full-byte read before masking");
  ++checked;
  ObservedCore missing;
  missing.set_registers(seed);
  const auto failure = missing.run_instruction(branch);
  require(failure.retired == 0 && failure.fault == Fault::memory_read &&
              failure.read_mask == branch[3] && missing.registers() == seed &&
              missing.statistics().read_callbacks == 0,
          "Missing selected and ordinary backends fail instead of fabricating a value");
}

void partial_knowledge() {
  Bus bus{.value = mixed_input, .known = 0x0f};
  Io io{.owner = &bus, .read = Bus::read, .read_masked = Bus::masked};
  ObservedCore core(Memory::external(), &io);
  core.set_registers(seed);
  auto wanted = seed;
  wanted.pc = static_cast<Address>(seed.pc + branch.size());
  require(core.run_instruction(branch).retired == 1 && core.registers() == wanted &&
              bus.selected == 1 && bus.full == 0,
          "A GPIO branch can observe known low bits without inventing high-bit knowledge");
  ++checked;
  core.set_registers(seed);
  constexpr std::array<Byte, 3> load{0xb6, 0xab, 0xcd};
  require(core.run_instruction(load).fault == Fault::memory_read && bus.full == 1 &&
              bus.selected == 1,
          "A full load still requires knowledge of every byte bit");
  core.set_registers(seed);
  constexpr std::array<Byte, 4> modify{0x1c, 0xab, 0xcd, 4};
  require(
      core.run_instruction(modify).fault == Fault::memory_read && bus.full == 2 &&
          bus.selected == 1 && bus.writes == 0 && core.registers() == seed,
      "Read-modify-write requires the original full byte to preserve unselected bits and flags");
}

void read_modify_write() {
  constexpr std::array<std::array<Byte, 3>, 4> cases{
      {{0x1c, 0x40, 0xe5}, {0x1d, 4, 0xa1}, {0x1c, 0, 0xa5}, {0x1d, 0xff, 0}}};
  for (const auto scene : cases) {
    Bus bus{.value = mixed_input};
    Io io{.owner = &bus, .read = Bus::read, .write = Bus::write, .read_masked = Bus::masked};
    ObservedCore core(Memory::external(), &io);
    const std::array<Byte, 4> code{scene[0], 0xab, 0xcd, scene[1]};
    auto before = seed;
    constexpr Byte all_flags = 0xff;
    before.ccr = all_flags;
    core.set_registers(before);
    auto wanted = before;
    constexpr unsigned upper_carry = 0xf1;
    wanted.ccr = static_cast<Byte>(upper_carry | (scene[2] == 0 ? 4U : 8U));
    wanted.pc = static_cast<Address>(seed.pc + code.size());
    const auto result = core.run_instruction(code);
    require(result.retired == 1 && result.fault == Fault::none && core.registers() == wanted &&
                bus.full == 1 && bus.selected == 0 && bus.writes == 1 &&
                bus.written.address == port && bus.written.value == scene[2],
            "BSET/BCLR preserve full-byte data/flags and never route through a selected-read hook");
    ++checked;
  }
}

void known_ram() {
  std::array<Byte, address_bytes> image{};
  constexpr Byte data = 0xf0;
  image[port] = data;
  Bus bus{.allowed = false};
  Io io{.owner = &bus, .read = Bus::read, .read_masked = Bus::masked};
  ObservedCore core(Memory::unified(image), &io);
  core.set_registers(seed);
  constexpr std::array<Byte, 5> code{0x1f, 0xab, 0xcd, 0x0f, 2};
  auto wanted = seed;
  wanted.pc = static_cast<Address>(seed.pc + code.size() + 2);
  const auto result = core.run_instruction(code);
  require(result.retired == 1 && result.fault == Fault::none && core.registers() == wanted &&
              bus.full == 0 && bus.selected == 0 && core.statistics().read_callbacks == 0,
          "Known RAM bit branches retain their direct native path despite refusing bus hooks");
  ++checked;
}
}

void boundary_cases() {
  refused();
  fault_masks();
  ordinary_fallback();
  partial_knowledge();
  read_modify_write();
  known_ram();
}
}
