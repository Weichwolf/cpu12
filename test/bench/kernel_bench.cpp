// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/isa.hpp"
#include "cpu12/memory.hpp"
#include "kernel_host.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {
using namespace cpu12;
using Clock = std::chrono::steady_clock;
constexpr Address entry = 0x0200, stack = 0x9000, data = 0x4000;
constexpr unsigned iterations = 128, repetitions = 128, trials = 9;

constexpr std::array<Byte, 2> add_operand{0, 3};
constexpr std::array<Byte, 1> xor_value{0x5a}, alu_backedge{0xf6};
constexpr auto arithmetic = isa::program(isa::opcode<isa::Opcode::addd_immediate>(),
                                         add_operand,
                                         isa::opcode<isa::Opcode::eora_immediate>(),
                                         xor_value,
                                         isa::opcode<isa::Opcode::eora_immediate>(),
                                         xor_value,
                                         isa::opcode<isa::Opcode::dex>(),
                                         isa::opcode<isa::Opcode::bne_relative>(),
                                         alu_backedge,
                                         isa::stop);
constexpr std::array<Byte, 2> data_operand{0x40, 0};
constexpr std::array<Byte, 1> increment{1}, memory_backedge{0xf5}, branch_backedge{0xfd};
constexpr auto memory = isa::program(isa::opcode<isa::Opcode::ldaa_extended>(),
                                     data_operand,
                                     isa::opcode<isa::Opcode::adda_immediate>(),
                                     increment,
                                     isa::opcode<isa::Opcode::staa_extended>(),
                                     data_operand,
                                     isa::opcode<isa::Opcode::dex>(),
                                     isa::opcode<isa::Opcode::bne_relative>(),
                                     memory_backedge,
                                     isa::stop);
constexpr auto branches = isa::program(isa::opcode<isa::Opcode::dex>(),
                                       isa::opcode<isa::Opcode::bne_relative>(),
                                       branch_backedge,
                                       isa::stop);

struct Fixture {
  std::string_view name;
  std::span<const Byte> program;
  std::uint32_t instructions;
  Address expected_d;
  Byte expected_memory;
  Byte expected_ccr;
  test::host::Result (*host)(Byte &);
};

constexpr std::array fixtures{Fixture{.name = "alu-flags",
                                      .program = arithmetic,
                                      .instructions = (iterations * 5) + 1,
                                      .expected_d = iterations * 3,
                                      .expected_memory = 0,
                                      .expected_ccr = 4,
                                      .host = test::host::arithmetic},
                              Fixture{.name = "ram-rmw",
                                      .program = memory,
                                      .instructions = (iterations * 5) + 1,
                                      .expected_d = 0x8000,
                                      .expected_memory = iterations,
                                      .expected_ccr = 0x2c,
                                      .host = test::host::memory},
                              Fixture{.name = "counted-branch",
                                      .program = branches,
                                      .instructions = (iterations * 2) + 1,
                                      .expected_d = 0,
                                      .expected_memory = 0,
                                      .expected_ccr = 4,
                                      .host = test::host::branches}};

void execute(Core &core, std::array<Byte, address_bytes> &ram, const Fixture &fixture) {
  ram[data] = 0;
  core.set_registers({.pc = entry, .x = iterations, .sp = stack});
  const auto result = core.run();
  const auto state = core.state();
  constexpr Address frame_bytes = 9;
  if (result.fault != Fault::none || result.retired != fixture.instructions ||
      state.execution != ExecutionState::stopped || state.registers.d != fixture.expected_d ||
      state.registers.x != 0 || state.registers.y != 0 ||
      state.registers.pc != entry + fixture.program.size() ||
      state.registers.sp != stack - frame_bytes || state.registers.ccr != fixture.expected_ccr ||
      ram[data] != fixture.expected_memory) {
    throw std::runtime_error("Guest benchmark failed independent result expectations");
  }
}

void execute_host(std::array<Byte, address_bytes> &ram, const Fixture &fixture) {
  ram[data] = 0;
  const auto result = fixture.host(ram[data]);
  if (result.d != fixture.expected_d || result.x != 0 || result.ccr != fixture.expected_ccr ||
      ram[data] != fixture.expected_memory) {
    throw std::runtime_error("Host ASM benchmark failed independent result expectations");
  }
  constexpr unsigned byte_bits = 8;
  constexpr unsigned byte_mask = 0xff;
  const auto return_pc = static_cast<Address>(entry + fixture.program.size());
  const std::array<Byte, 9> frame{result.ccr,
                                  static_cast<Byte>(result.d & byte_mask),
                                  static_cast<Byte>(result.d >> byte_bits),
                                  static_cast<Byte>(result.x >> byte_bits),
                                  static_cast<Byte>(result.x & byte_mask),
                                  0,
                                  0,
                                  static_cast<Byte>(return_pc >> byte_bits),
                                  static_cast<Byte>(return_pc & byte_mask)};
  std::ranges::copy(frame, ram.begin() + stack - frame.size());
}

template <class Operation> double median(Operation operation) {
  std::array<double, trials> samples{};
  for (auto &sample : samples) {
    const auto start = Clock::now();
    for (unsigned index = 0; index < repetitions; ++index) { operation(); }
    sample = std::chrono::duration<double>(Clock::now() - start).count();
  }
  std::ranges::sort(samples);
  return samples[trials / 2];
}

void measure(const Fixture &fixture) {
  std::array<Byte, address_bytes> ram{};
  std::ranges::copy(fixture.program, ram.begin() + entry);
  auto host_ram = ram;
  const auto code_end = static_cast<Address>(entry + fixture.program.size());
  const std::array regions{Ram{.first = 0, .bytes = std::span(ram).first(entry)},
                           Ram{.first = entry,
                               .bytes = std::span(ram).subspan(entry, fixture.program.size()),
                               .writable = false},
                           Ram{.first = code_end, .bytes = std::span(ram).subspan(code_end)}};
  Core core(Memory::mapped(regions));
  const auto cold_start = Clock::now();
  execute(core, ram, fixture);
  const auto cold = Clock::now() - cold_start;
  execute_host(host_ram, fixture);
  if (host_ram != ram) { throw std::runtime_error("Host ASM and JIT memory effects differ"); }
  const auto seconds = median([&] { execute(core, ram, fixture); });
  const auto host_seconds = median([&] { execute_host(host_ram, fixture); });
  if (host_ram != ram) { throw std::runtime_error("Warm host ASM and JIT memory effects differ"); }
  constexpr double million = 1e6;
  constexpr double percent = 100;
  constexpr double nanoseconds_per_second = 1e9;
  std::cout << fixture.name
            << " cold_run_us=" << std::chrono::duration<double, std::micro>(cold).count()
            << " warm_median_ns_per_instruction="
            << (seconds * nanoseconds_per_second) / (fixture.instructions * repetitions)
            << " warm_mips=" << (fixture.instructions * repetitions) / seconds / million
            << " host_asm_ns_per_instruction="
            << (host_seconds * nanoseconds_per_second) / (fixture.instructions * repetitions)
            << " jit_to_host_percent=" << (host_seconds / seconds) * percent << '\n';
}
}

int main() {
  try {
    // Warm measurements include register/input reset and correctness verification.
    std::cout << "comparison=asm_with_stop_vs_core warm_scope=reset+run+verify trials=" << trials
              << " repetitions=" << repetitions << '\n';
    for (const auto &fixture : fixtures) { measure(fixture); }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
