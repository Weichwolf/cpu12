// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction_image.hpp"
#include "isa.hpp"
#include "masked_read_model.hpp"

#include <array>
#include <span>
#include <vector>

namespace cpu12::test::masked {
namespace {
using cpu12::test::InstructionCore;
constexpr auto branch = isa::program(isa::opcode<isa::Opcode::brset_extended>(),
                                     std::array<Byte, 4>{0xab, 0xcd, 0x0f, 0});
constexpr auto full_read =
    isa::program(isa::opcode<isa::Opcode::ldaa_extended>(), std::array<Byte, 2>{0xab, 0xcd});
constexpr auto modify =
    isa::program(isa::opcode<isa::Opcode::bset_extended>(), std::array<Byte, 3>{0xab, 0xcd, 4});
constexpr std::array positions{seed.pc};

std::vector<Byte> operation(const std::span<const Byte> bytes) {
  return {bytes.begin(), bytes.end()};
}

RunResult execute(InstructionCore &core, const std::span<const Byte> code) {
  core.select(code, core.registers().pc);
  return core.run(1);
}

constexpr Address port = 0xabcd;
constexpr Byte mixed_input = 0xa5;
constexpr Byte fallback_input = 0x85;

void refused() {
  Bus bus{.value = mixed_input, .reject_selected = true};
  Io io{.owner = &bus, .read = Bus::read, .read_masked = Bus::masked};
  const std::array operations{operation(branch), operation(full_read), operation(isa::daa)};
  InstructionCore core(operations, positions, &io);
  core.set_registers(seed);
  const auto result = execute(core, branch);
  require(result.retired == 0 && result.fault == Fault::memory_read && result.pc == seed.pc &&
              result.address == port && result.value == mixed_input && result.access_size == 1 &&
              result.read_mask == branch[3] && core.registers() == seed && bus.selected == 1 &&
              bus.full == 0 && bus.value == 0,
          "Refused selected reads retain the actual diagnostic value and never retry a full read");
  require(execute(core, branch).fault == Fault::memory_read && bus.selected == 1 && bus.full == 0,
          "Masked-read failures latch without repeating destructive side effects");
  core.set_registers(seed);
  bus.allowed = false;
  const auto ordinary = execute(core, full_read);
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
  const auto unqualified = execute(core, isa::daa);
  require(unqualified.fault == Fault::unsupported_state && unqualified.read_mask == 0 &&
              unqualified.access_size == 0 && unqualified.address == 0 &&
              core.registers() == decimal,
          "An explicit boundary prevents old selected-bit metadata leaking into a nonmemory fault");
}

void fault_masks() {
  Bus bus{.reject_selected = true};
  Io io{.owner = &bus, .read = Bus::read, .read_masked = Bus::masked};
  constexpr unsigned maximum = 0xff;
  std::vector<std::vector<Byte>> operations;
  for (unsigned mask = 0; mask <= maximum; ++mask) {
    auto code = branch;
    code[3] = static_cast<Byte>(mask);
    operations.push_back(operation(code));
  }
  InstructionCore core(operations, positions, &io);
  constexpr Byte rejected_value = 0xa5;
  for (unsigned mask = 0; mask <= maximum; ++mask) {
    auto code = branch;
    code[3] = static_cast<Byte>(mask);
    bus.value = rejected_value;
    core.set_registers(seed);
    const auto result = execute(core, code);
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
  const std::array operations{operation(branch), operation(full_read), operation(modify)};
  InstructionCore core(operations, positions, &io);
  core.set_registers(seed);
  auto wanted = seed;
  wanted.pc = static_cast<Address>(seed.pc + branch.size());
  const auto result = execute(core, branch);
  require(result.retired == 1 && result.fault == Fault::none && core.registers() == wanted &&
              bus.full == 1 && bus.selected == 0 && bus.value == 0,
          "Absent selected interface uses one actual qualified full-byte read before masking");
  ++checked;
  InstructionCore missing(operations, positions);
  missing.set_registers(seed);
  const auto failure = execute(missing, branch);
  require(failure.retired == 0 && failure.fault == Fault::memory_read &&
              failure.read_mask == branch[3] && missing.registers() == seed,
          "Missing selected and ordinary backends fail instead of fabricating a value");
}

void partial_knowledge() {
  Bus bus{.value = mixed_input, .known = 0x0f};
  Io io{.owner = &bus, .read = Bus::read, .read_masked = Bus::masked};
  const std::array operations{operation(branch), operation(full_read), operation(modify)};
  InstructionCore core(operations, positions, &io);
  core.set_registers(seed);
  auto wanted = seed;
  wanted.pc = static_cast<Address>(seed.pc + branch.size());
  require(execute(core, branch).retired == 1 && core.registers() == wanted && bus.selected == 1 &&
              bus.full == 0,
          "A GPIO branch can observe known low bits without inventing high-bit knowledge");
  ++checked;
  core.set_registers(seed);
  require(execute(core, full_read).fault == Fault::memory_read && bus.full == 1 &&
              bus.selected == 1,
          "A full load still requires knowledge of every byte bit");
  core.set_registers(seed);
  require(
      execute(core, modify).fault == Fault::memory_read && bus.full == 2 && bus.selected == 1 &&
          bus.writes == 0 && core.registers() == seed,
      "Read-modify-write requires the original full byte to preserve unselected bits and flags");
}

void read_modify_write() {
  constexpr std::array<std::array<Byte, 3>, 4> cases{
      {{static_cast<Byte>(isa::Opcode::bset_extended), 0x40, 0xe5},
       {static_cast<Byte>(isa::Opcode::bclr_extended), 4, 0xa1},
       {static_cast<Byte>(isa::Opcode::bset_extended), 0, 0xa5},
       {static_cast<Byte>(isa::Opcode::bclr_extended), 0xff, 0}}};
  for (const auto scene : cases) {
    Bus bus{.value = mixed_input};
    Io io{.owner = &bus, .read = Bus::read, .write = Bus::write, .read_masked = Bus::masked};
    const std::array<Byte, 4> code{scene[0], 0xab, 0xcd, scene[1]};
    const std::array operations{operation(code)};
    InstructionCore core(operations, positions, &io);
    auto before = seed;
    constexpr Byte all_flags = 0xff;
    before.ccr = all_flags;
    core.set_registers(before);
    auto wanted = before;
    constexpr unsigned upper_carry = 0xf1;
    wanted.ccr = static_cast<Byte>(upper_carry | (scene[2] == 0 ? 4U : 8U));
    wanted.pc = static_cast<Address>(seed.pc + code.size());
    const auto result = execute(core, code);
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
  constexpr auto code = isa::program(isa::opcode<isa::Opcode::brclr_extended>(),
                                     std::array<Byte, 4>{0xab, 0xcd, 0x0f, 2});
  const std::array operations{operation(code)};
  const std::array<Address, 1> landings{static_cast<Address>(seed.pc + code.size() + 2)};
  const std::array data_grants{Ram{.first = port, .bytes = std::span(image).subspan(port, 1)}};
  InstructionCore core(operations, positions, &io, landings, data_grants);
  core.set_registers(seed);
  auto wanted = seed;
  wanted.pc = static_cast<Address>(seed.pc + code.size() + 2);
  const auto result = execute(core, code);
  require(result.retired == 1 && result.fault == Fault::none && core.registers() == wanted &&
              bus.full == 0 && bus.selected == 0,
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
