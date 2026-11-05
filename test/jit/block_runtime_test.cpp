// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr Address entry = 0x0200, stack = 0x9000;

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Device {
  std::array<Byte, address_bytes> ram{};
  std::array<Ram, 3> regions;
  Core core;

  explicit Device(const std::span<const Byte> program)
      : regions{Ram{.first = 0, .bytes = std::span(ram).first(entry)},
                Ram{.first = entry,
                    .bytes = std::span(ram).subspan(entry, program.size()),
                    .writable = false},
                Ram{.first = static_cast<Address>(entry + program.size()),
                    .bytes = std::span(ram).subspan(entry + program.size())}},
        core(Memory::mapped(regions)) {
    std::ranges::copy(program, ram.begin() + entry);
    core.set_registers({.pc = entry, .sp = stack});
  }
};

void long_region() {
  constexpr unsigned count = 513;
  std::array<Byte, count + isa::stop.size()> program{};
  program.fill(isa::nop[0]);
  std::ranges::copy(isa::stop, program.begin() + count);
  Device device(program);
  const auto result = device.core.run();
  require(result.fault == Fault::none && result.retired == count + 1 &&
              result.state == ExecutionState::stopped && result.pc == entry + count &&
              result.opcode == isa::stop[0] &&
              device.core.registers().pc == entry + program.size() &&
              device.core.statistics().compiled_entries == 1,
          "A complete natural region exceeds fixed translation caps and retires through STOP");
  const auto registers = device.core.registers();
  const auto ram = device.ram;
  device.core.set_registers({.pc = entry, .sp = stack});
  const auto repeated = device.core.run();
  require(repeated == result && device.core.registers() == registers && device.ram == ram &&
              device.core.statistics().compiled_entries == 1,
          "Repeated execution reuses one retained native region with identical effects");
}

void bounded_loop() {
  constexpr std::array<Byte, 1> displacement{0xfd};
  constexpr auto program = isa::program(isa::opcode<isa::Opcode::dex>(),
                                        isa::opcode<isa::Opcode::bne_relative>(),
                                        displacement,
                                        isa::stop);
  Device device(program);
  constexpr Address iterations = 300;
  device.core.set_registers({.pc = entry, .x = iterations, .sp = stack});
  constexpr unsigned limit = 127;
  const auto first = device.core.run(limit);
  require(first.fault == Fault::none && first.retired == limit && first.pc == entry &&
              first.opcode == isa::opcode<isa::Opcode::dex>()[0] &&
              device.core.registers().x == iterations - (limit + 1) / 2 &&
              device.core.registers().pc == entry + isa::opcode<isa::Opcode::dex>().size(),
          "A grant ending inside a native loop stops after the exact instruction");
  const auto remaining = device.core.run();
  require(remaining.fault == Fault::none && remaining.retired == iterations * 2 + 1 - limit &&
              remaining.state == ExecutionState::stopped && device.core.registers().x == 0 &&
              device.core.registers().ccr == 4 && device.core.registers().sp == stack - 9,
          "A native backedge resumes with exact flags and original STOP stacking");
}

void precise_fault() {
  constexpr std::array<Byte, 1> invalid_accumulator{0xfa};
  constexpr Address invalid_d = 0xfa00;
  constexpr auto program = isa::program(isa::nop,
                                        isa::opcode<isa::Opcode::ldaa_immediate>(),
                                        invalid_accumulator,
                                        isa::daa,
                                        isa::stop);
  Device device(program);
  device.core.set_registers({.pc = entry, .sp = stack, .ccr = 0x20});
  const auto result = device.core.run();
  const auto state = device.core.state();
  require(result.fault == Fault::unsupported_state && result.retired == 2 &&
              result.pc == entry + 3 && result.opcode == isa::daa[0] &&
              state.registers.pc == entry + 3 && state.registers.d == invalid_d &&
              state.registers.sp == stack,
          "A joined native fault preserves the precise failing PC and preceding retirements");
  const auto again = device.core.run();
  require(again.fault == result.fault && again.retired == 0 && device.core.state() == state,
          "A latched native failure cannot replay preceding work");
}
}

int main() {
  try {
    long_region();
    bounded_loop();
    precise_fault();
    std::cout << "Native region length, reuse, bounded backedges and precise faults passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
