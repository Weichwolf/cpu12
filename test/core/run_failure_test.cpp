// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "observed_core.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
constexpr Address entry = 0x0200, second = 0x0210, data = 0x4000, offered = 0x8001;
constexpr unsigned byte_bits = 8, byte_mask = 0xff;
constexpr Registers initial{.pc = entry, .d = 0x1122, .ccr = 0xff};
constexpr Byte load = static_cast<Byte>(isa::Opcode::ldd_extended);
constexpr Byte recovered_ccr = 0xf9;
constexpr std::array<Byte, 2> target{second >> byte_bits, second &byte_mask};
constexpr std::array<Byte, 2> address{data >> byte_bits, data &byte_mask};
constexpr auto prefix =
    isa::program(isa::nop, isa::nop, isa::nop, isa::opcode<isa::Opcode::jmp_extended>(), target);
constexpr auto body = isa::program(isa::opcode<isa::Opcode::ldd_extended>(), address, isa::nop);

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Port {
  unsigned reads = 0;
  bool accept = false, correct_address = true;

  static bool read(void *owner, const Address actual, Address &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    ++port.reads;
    port.correct_address = port.correct_address && actual == data;
    value = offered;
    return port.accept;
  }
};

void exercise() {
  auto first_code = prefix;
  auto second_code = body;
  const std::array regions{Ram{.first = entry, .bytes = first_code, .writable = false},
                           Ram{.first = second, .bytes = second_code, .writable = false}};
  Port port;
  Io io{.owner = &port, .read_word = Port::read};
  ObservedCore core(Memory::mapped(regions), &io);
  constexpr RunResult failed{.retired = 4,
                             .fault = Fault::memory_read,
                             .pc = second,
                             .opcode = load,
                             .address = data,
                             .value = offered,
                             .access_size = 2};
  auto latched = failed;
  latched.retired = 0;
  auto failed_registers = initial;
  failed_registers.pc = second;
  // Cold dispatch stops at an uncompiled target; warm dispatch enters both
  // retained blocks. Neither may lose prior progress or replay the failed read.
  for (unsigned attempt = 1; attempt <= 2; ++attempt) {
    core.set_registers(initial);
    require(core.run() == failed && core.registers() == failed_registers &&
                core.state().failure == latched && port.reads == attempt,
            "Cold/warm block failure must preserve progress, exact metadata and its latch");
    const auto state = core.state();
    require(core.run() == latched && core.run(0) == latched && core.step() == latched &&
                core.run_instruction(isa::nop) == latched && core.state() == state &&
                port.reads == attempt,
            "Repeated run/step/captured entry cannot replay a latched I/O fault");
  }
  port.accept = true;
  core.set_registers(initial);
  constexpr RunResult succeeded{.retired = 6, .pc = second + 3, .opcode = isa::nop[0]};
  auto recovered = initial;
  recovered.pc = second + body.size();
  recovered.d = offered;
  recovered.ccr = recovered_ccr; // LDD supplies N, clears Z/V, preserves the other bits.
  require(core.run(6) == succeeded && core.registers() == recovered &&
              core.state().failure == RunResult{} && port.reads == 3 && port.correct_address &&
              core.statistics().native_instructions == 14,
          "Recovered success must clear public fault metadata despite stale context details");
  require(first_code == prefix && second_code == body,
          "Dispatch and I/O failures must not modify the original program");
}
}

int main() {
  try {
    exercise();
    std::cout << "Cold/warm progress, failure latches and recovered result metadata passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
