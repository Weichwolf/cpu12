// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "preparation_registers.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace cpu12::test::prepared_registers {
namespace {
constexpr unsigned irq = 0x10, xirq = 0x40;

void readiness(Core &core, const State &state) {
  require(core.interrupt_ready(InterruptKind::maskable) ==
                  ((state.registers.ccr & irq) == 0 && state.irq_delay == 0) &&
              core.interrupt_ready(InterruptKind::xirq) == ((state.registers.ccr & xirq) == 0),
          "Prepared CCR operations preserve actual maskable/XIRQ eligibility and delay");
}

struct Device {
  Core *core = nullptr;
  State before;
  Progress progress;
  bool accept = false, yield = false, observed = false;
  unsigned calls = 0;
  static constexpr Byte value = 0x80;
  static constexpr Address address = 0x2000;

  static bool read(void *owner, const Address at, Byte &result) noexcept {
    auto &device = *static_cast<Device *>(owner);
    ++device.calls;
    result = value;
    device.observed = at == address && device.core->state() == device.before &&
                      device.core->progress() == device.progress;
    if (device.yield) { device.core->request_yield(); }
    return device.accept;
  }
};

void delayed_io() {
  constexpr auto keep_irq = static_cast<Byte>(byte_mask ^ irq);
  constexpr auto bytes = isa::program(isa::opcode<isa::Opcode::andcc_immediate>(),
                                      std::array{keep_irq},
                                      isa::opcode<isa::Opcode::ldaa_extended>(),
                                      std::array{static_cast<Byte>(Device::address >> byte_bits),
                                                 static_cast<Byte>(Device::address)},
                                      isa::opcode<isa::Opcode::bra_relative>(),
                                      std::array<Byte, 1>{0xf9});
  for (const bool accept : {false, true}) {
    for (const bool yield : {false, true}) {
      auto program = bytes;
      Device device{.before = {}, .progress = {}, .accept = accept, .yield = yield};
      Io io{.owner = &device, .read = Device::read};
      Core core(Memory::window({.first = entry, .bytes = program, .writable = false}), &io);
      device.core = &core;
      constexpr std::array entries{entry};
      const auto report = core.prepare(entries);
      require(report.prepared_entries == 3 && report.unresolved_entries == 0 &&
                  report.native_bytes > 0 && report.failure.fault == Fault::none &&
                  device.calls == 0,
              "CCR and following I/O prepare without executing either instruction");
      const auto compilations = core.statistics().compiled_entries;
      auto seed = base;
      seed.pc = entry;
      seed.ccr = irq | xirq;
      core.set_registers(seed);
      State expected{.registers = seed, .failure = {}, .irq_delay = 1};
      expected.registers.pc = entry + 2;
      expected.registers.ccr = xirq;
      require(core.run(1).retired == 1 && core.state() == expected &&
                  core.progress() == Progress{.estimated_cycles = 1},
              "Prepared IRQ unmask exposes exactly one deferred instruction");
      device.before = expected;
      device.progress = core.progress();
      constexpr unsigned deadline = 100;
      const auto result = core.run_until(deadline);
      if (accept) {
        expected.registers.pc = entry + 5;
        expected.registers.d =
            static_cast<Address>((Device::value << byte_bits) | (seed.d & byte_mask));
        expected.registers.ccr = xirq | 8U;
        expected.irq_delay = 0;
      } else {
        expected.failure = {.fault = Fault::memory_read,
                            .pc = entry + 2,
                            .opcode = static_cast<Byte>(isa::Opcode::ldaa_extended),
                            .address = Device::address,
                            .value = Device::value,
                            .access_size = 1,
                            .read_mask = byte_mask};
      }
      require(result.retired == (accept ? 1U : 0U) && core.state() == expected &&
                  core.progress() == Progress{.estimated_cycles = accept ? 4U : 1U} &&
                  device.calls == 1 && device.observed &&
                  core.statistics().compiled_entries == compilations && program == bytes,
              "Deferred prepared I/O preserves atomic fault/yield timing and never triggers lazy "
              "ISA compilation");
    }
  }
}
}

std::size_t ccr_cases() {
  std::vector<Encoding> codes;
  constexpr std::size_t operand_count = byte_mask + 1;
  codes.reserve(2 * operand_count);
  for (const auto opcode : {isa::Opcode::andcc_immediate, isa::Opcode::orcc_immediate}) {
    for (unsigned operand = 0; operand <= byte_mask; ++operand) {
      codes.push_back(
          {.bytes = {static_cast<Byte>(opcode), static_cast<Byte>(operand)}, .length = 2});
    }
  }
  Pair pair(codes);
  std::size_t count = 0;
  for (std::size_t index = 0; index < codes.size(); ++index) {
    const auto encoding = codes[index];
    const bool conjunction = index < operand_count;
    const unsigned operand = encoding.bytes[1];
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      auto seed = base;
      seed.pc = pair.program.entries[index];
      seed.ccr = static_cast<Byte>(ccr);
      State wanted{.registers = seed, .failure = {}};
      wanted.registers.pc = static_cast<Address>(seed.pc + encoding.length);
      wanted.registers.ccr =
          static_cast<Byte>(conjunction ? ccr & operand : ccr | (operand & (byte_mask ^ xirq)));
      wanted.irq_delay =
          static_cast<Byte>((ccr & irq) != 0 && (wanted.registers.ccr & irq) == 0 ? 1 : 0);
      check(pair, seed, encoding, wanted, 1);
      readiness(pair.prepared, wanted);
      ++count;
      constexpr std::array<Byte, 8> edges{0, 1, 0x10, 0x40, 0x50, 0x7f, 0xbf, 0xff};
      for (const auto selected : edges) {
        if (ccr == selected) { prefixes(pair, seed); }
      }
    }
  }
  pair.unchanged();
  delayed_io();
  return count;
}
}
