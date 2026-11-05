// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "jit/arena.hpp"
#include "jit/block.hpp"
#include "kernel_host.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
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

void verify(const State &state,
            const RunResult result,
            const Fixture &fixture,
            const std::array<Byte, address_bytes> &ram) {
  constexpr Address frame_bytes = 9;
  if (result.fault != Fault::none || result.retired != fixture.instructions ||
      state.execution != ExecutionState::stopped || state.registers.d != fixture.expected_d ||
      state.registers.x != 0 || state.registers.y != 0 ||
      state.registers.pc != entry + fixture.program.size() ||
      state.registers.sp != stack - frame_bytes || state.registers.ccr != fixture.expected_ccr ||
      ram[data] != fixture.expected_memory) {
    std::cerr << "fixture=" << fixture.name << " fault=" << static_cast<unsigned>(result.fault)
              << " retired=" << result.retired << " d=" << state.registers.d
              << " x=" << state.registers.x << " pc=" << state.registers.pc
              << " sp=" << state.registers.sp << " ccr=" << unsigned{state.registers.ccr}
              << " memory=" << unsigned{ram[data]} << '\n';
    throw std::runtime_error("Guest benchmark failed independent result expectations");
  }
}

void execute(Core &core, std::array<Byte, address_bytes> &ram, const Fixture &fixture) {
  ram[data] = 0;
  core.set_registers({.pc = entry, .x = iterations, .sp = stack});
  const auto result = core.run();
  verify(core.state(), result, fixture, ram);
}

// Test-only use of the production parser/emitter/entry bridge; no Core API is added.
class NativeRun {
public:
  NativeRun(const Memory &memory_map, const Fixture &fixture) {
    const auto stop = static_cast<Address>(entry + fixture.program.size() - isa::stop.size());
    const std::array entries{entry, stop};
    for (std::size_t index = 0; index < entries.size(); ++index) {
      const auto instructions = detail::parse_block(memory_map, entries[index]);
      if (instructions.empty()) { throw std::runtime_error("Missing native benchmark region"); }
      const auto bytes = detail::emit_block(instructions, memory_map);
      code_bytes_ += bytes.size();
      blocks[index] = arena.append(bytes);
    }
  }

  void execute(std::array<Byte, address_bytes> &ram, const Fixture &fixture) {
    ram[data] = 0;
    context.cpu = {};
    context.cpu.registers = {.pc = entry, .x = iterations, .sp = stack};
    context.fault = Fault::none;
    constexpr auto budget = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t retired = 0;
    for (const auto *block : blocks) {
      retired += detail::enter_block(block, context.cpu.registers, context, budget - retired);
      if (context.fault != Fault::none || context.cpu.execution != ExecutionState::running) {
        break;
      }
    }
    verify(context.cpu, {.retired = retired, .fault = context.fault}, fixture, ram);
  }

  [[nodiscard]] const State &state() const { return context.cpu; }

  [[nodiscard]] std::size_t code_bytes() const { return code_bytes_; }

private:
  std::size_t code_bytes_ = 0;
  detail::ExecutableArena arena;
  detail::NativeContext context{};
  std::array<const void *, 2> blocks{};
};

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

void verify_arithmetic_host() {
  constexpr unsigned word_mask = 0xffff;
  constexpr unsigned sign = 0x8000;
  constexpr unsigned zero = 4;
  constexpr unsigned negative = 8;
  constexpr std::array<Address, 12> values{
      0, 1, 0x7ffc, 0x7ffd, 0x7ffe, 0x7fff, 0x8000, 0xfffb, 0xfffc, 0xfffd, 0xfffe, 0xffff};
  constexpr std::array<Address, 3> counts{1, 2, iterations};
  for (const auto value : values) {
    for (const auto count : counts) {
      test::host::KernelRegisters state{.value = value, .count = count};
      test::host::arithmetic_kernel(state);
      const auto result = test::host::arithmetic_result(state);
      const auto before_last = (unsigned{value} + 3U * (unsigned{count} - 1U)) & word_mask;
      const auto expected = (before_last + 3U) & word_mask;
      const auto ccr = zero | (expected >= sign ? negative : 0U) |
                       static_cast<unsigned>(before_last + 3U > word_mask);
      if (result.d != expected || result.x != 0 || result.ccr != ccr) {
        throw std::runtime_error("Host ALU ASM disagrees with independent modular sum/final flags");
      }
    }
  }
}

void verify_memory_host() {
  constexpr unsigned byte_modulus = 256;
  constexpr unsigned nibble_modulus = 16;
  constexpr unsigned sign = 128;
  constexpr unsigned negative = 8;
  constexpr unsigned half = 0x20;
  constexpr unsigned zero = 4;
  constexpr std::array<Address, 3> counts{1, 2, iterations};
  for (unsigned value = 0; value < byte_modulus; ++value) {
    for (const auto count : counts) {
      auto payload = static_cast<Byte>(value);
      test::host::KernelRegisters state{.count = count};
      test::host::memory_kernel(state, payload);
      const auto result = test::host::memory_result(state);
      const auto before_last = (value + count - 1U) % byte_modulus;
      const auto expected = (before_last + 1U) % byte_modulus;
      const auto ccr = zero | (expected >= sign ? negative : 0U) |
                       ((((before_last % nibble_modulus) + 1U) / nibble_modulus) * half) |
                       ((before_last + 1U) / byte_modulus);
      if (payload != expected || result.d != expected * byte_modulus || result.x != 0 ||
          result.ccr != ccr) {
        throw std::runtime_error("Host RAM ASM disagrees with independent byte sum/final flags");
      }
    }
  }
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
  const auto memory_map = Memory::mapped(regions);
  Core core(memory_map);
  const auto cold_start = Clock::now();
  execute(core, ram, fixture);
  const auto cold = Clock::now() - cold_start;
  NativeRun native(memory_map, fixture);
  const auto expected_ram = ram;
  native.execute(ram, fixture);
  if (native.state() != core.state() || ram != expected_ram) {
    throw std::runtime_error("Direct production native entries and Core effects differ");
  }
  execute_host(host_ram, fixture);
  if (host_ram != ram) { throw std::runtime_error("Host ASM and JIT memory effects differ"); }
  const auto seconds = median([&] { execute(core, ram, fixture); });
  const auto native_seconds = median([&] { native.execute(ram, fixture); });
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
            << " jit_to_host_percent=" << (host_seconds / seconds) * percent
            << " native_entries_to_host_percent=" << (host_seconds / native_seconds) * percent
            << " native_entries_ns_per_instruction="
            << (native_seconds * nanoseconds_per_second) / (fixture.instructions * repetitions)
            << " core_over_native_ns_per_run="
            << ((seconds - native_seconds) * nanoseconds_per_second) / repetitions
            << " native_code_bytes=" << native.code_bytes() << '\n';
}
}

int main() {
  try {
    verify_arithmetic_host();
    verify_memory_host();
    // Warm measurements include register/input reset and correctness verification.
    std::cout
        << "comparison=asm_with_stop_vs_core_and_native_entries warm_scope=reset+run+verify trials="
        << trials << " repetitions=" << repetitions << '\n';
    for (const auto &fixture : fixtures) { measure(fixture); }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
