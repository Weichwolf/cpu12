// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "jit/arena.hpp"
#include "jit/block.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr Address first = 0x0200, second = 0x0400, third = 0x0600, stack = 0x9000;
constexpr Address failing_port = 0x0300, failing_stack = 0x0500;
constexpr Address first_d = 0x7f00;
constexpr Byte completed_ccr = 0x2a;
constexpr std::array<Byte, 1> initial_a{0x7f}, increment{1};
constexpr std::array<Byte, 2> next_second{4, 0}, next_third{6, 0}, port{3, 0};
constexpr auto jump = isa::opcode<isa::Opcode::jmp_extended>();
constexpr auto program_a = isa::program(
    isa::opcode<isa::Opcode::ldaa_immediate>(), initial_a, isa::nop, jump, next_second);
constexpr auto program_b =
    isa::program(isa::opcode<isa::Opcode::adda_immediate>(), increment, jump, next_third);
constexpr auto fault_b = isa::program(isa::opcode<isa::Opcode::adda_immediate>(),
                                      increment,
                                      isa::opcode<isa::Opcode::staa_extended>(),
                                      port,
                                      jump,
                                      next_third);
constexpr std::array<Address, 3> addresses{first, second, third};
constexpr std::array<std::size_t, 3> page_numbers{2, 4, 6};
constexpr Registers seed{.pc = first, .x = 0x1234, .y = 0x5678, .sp = stack};
constexpr auto unlimited = std::numeric_limits<std::uint32_t>::max();

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct RunInput {
  std::uint32_t budget = unlimited;
  Address sp = stack;
};

struct Harness {
  std::array<Byte, address_bytes> ram{};
  std::array<detail::NativePage, addresses.size()> pages{};
  detail::NativeIndex index{};
  detail::NativeContext context{};
  detail::ExecutableArena arena;

  explicit Harness(const bool failing_write = false,
                   const std::span<const Byte> terminal = isa::stop) {
    std::ranges::copy(program_a, ram.begin() + first);
    const auto middle =
        failing_write ? std::span<const Byte>(fault_b) : std::span<const Byte>(program_b);
    std::ranges::copy(middle, ram.begin() + second);
    std::ranges::copy(terminal, ram.begin() + third);
    constexpr Address code_end = 0x0800;
    const std::array regions{Ram{.first = 0, .bytes = std::span(ram).first(first)},
                             Ram{.first = first,
                                 .bytes = std::span(ram).subspan(first, code_end - first),
                                 .writable = false},
                             Ram{.first = code_end, .bytes = std::span(ram).subspan(code_end)}};
    const auto memory = Memory::mapped(regions);
    for (std::size_t slot = 0; slot < addresses.size(); ++slot) {
      const auto instructions = detail::parse_block(memory, addresses[slot]);
      require(!instructions.empty(), "Authored native block must decode");
      pages[slot][0] = arena.append(detail::emit_block(instructions, memory));
      index[page_numbers[slot]] = pages[slot].data();
    }
  }

  detail::NativeResult run(const RunInput input = {}) {
    context = {};
    context.cpu.registers = seed;
    context.cpu.registers.sp = input.sp;
    return detail::enter_blocks(pages[0][0], context.cpu.registers, context, input.budget, index);
  }
};

void check(const detail::NativeResult result,
           const unsigned count,
           const detail::ExitLocation location) {
  require(result.retired == count && result.location.pc == location.pc &&
              result.location.opcode == location.opcode,
          "Native dispatch lost retirement progress or exact exit identity");
}

void complete() {
  Harness harness;
  const auto result = harness.run();
  check(result, 6, {.pc = third, .opcode = isa::stop[0]});
  auto expected = seed;
  expected.pc = third + isa::stop.size();
  expected.d = 0x8000;
  expected.sp = stack - 9;
  expected.ccr = completed_ccr;
  require(harness.context.fault == Fault::none && harness.context.cpu.registers == expected &&
              harness.context.cpu.execution == ExecutionState::stopped,
          "One native dispatch call must execute all three known blocks and original STOP");
  constexpr std::array<Byte, 9> frame{0x2a, 0, 0x80, 0x12, 0x34, 0x56, 0x78, 6, 2};
  require(
      std::ranges::equal(frame, std::span(harness.ram).subspan(stack - frame.size(), frame.size())),
      "Chained STOP must retain the independently specified original stack frame");
}

void boundaries() {
  Harness harness;
  const detail::ExitLocation first_exit{.pc = first + 3, .opcode = jump[0]};
  check(harness.run({.budget = 3}), 3, first_exit);
  require(harness.context.cpu.registers.pc == second && harness.context.cpu.registers.d == first_d,
          "Exact native budget ends before the second block");
  check(harness.run({.budget = 4}), 3, first_exit);
  require(harness.context.cpu.registers.pc == second && harness.context.cpu.registers.d == first_d,
          "An inadmissible next block must not replace the last executed exit location");
  check(harness.run({.budget = 5}), 5, {.pc = second + 2, .opcode = jump[0]});
  require(harness.context.cpu.registers.pc == third &&
              harness.context.cpu.execution == ExecutionState::running,
          "A zero remaining grant must not enter STOP");
  harness.index[page_numbers[1]] = nullptr;
  check(harness.run(), 3, first_exit);
  require(harness.context.fault == Fault::none && harness.context.cpu.registers.pc == second,
          "A missing target page exits without performing speculative target work");
  harness.index[page_numbers[1]] = harness.pages[1].data();
  harness.pages[1][0] = nullptr;
  check(harness.run(), 3, first_exit);
  require(harness.context.fault == Fault::none && harness.context.cpu.registers.pc == second,
          "A missing target entry exits for normal cold compilation");
}

void sleep_boundaries() {
  Harness harness(false, isa::wai);
  check(harness.run(), 6, {.pc = third, .opcode = isa::wai[0]});
  require(harness.context.cpu.execution == ExecutionState::waiting &&
              harness.context.cpu.registers.pc == third + isa::wai.size() &&
              harness.context.cpu.registers.sp == stack - 9,
          "A chained WAI must stop at the native wait boundary with its real stack frame");
  std::array<Byte, address_bytes> ram{};
  constexpr auto program = isa::program(isa::stop, isa::opcode<isa::Opcode::inx>(), isa::wai);
  std::ranges::copy(program, ram.begin() + first);
  const std::array regions{Ram{.first = 0, .bytes = std::span(ram).first(first)},
                           Ram{.first = first,
                               .bytes = std::span(ram).subspan(first, program.size()),
                               .writable = false},
                           Ram{.first = first + program.size(),
                               .bytes = std::span(ram).subspan(first + program.size())}};
  Core core(Memory::mapped(regions));
  constexpr Byte disable_stop = 0x80;
  auto expected = seed;
  expected.pc = first + program.size();
  ++expected.x;
  expected.sp = stack - 9;
  expected.ccr = disable_stop;
  for (unsigned repetition = 0; repetition < 2; ++repetition) {
    auto before = seed;
    before.ccr = disable_stop;
    core.set_registers(before);
    const auto result = core.run();
    require(result.retired == 3 && result.fault == Fault::none &&
                result.state == ExecutionState::waiting && result.opcode == isa::wai[0] &&
                result.pc == first + program.size() - isa::wai.size() &&
                core.registers() == expected,
            "Cold and retained execution must continue past disabled STOP to INX and WAI");
  }
}

void faults() {
  Harness stack_failure;
  check(stack_failure.run({.sp = failing_stack}), 5, {.pc = third, .opcode = isa::stop[0]});
  require(stack_failure.context.fault == Fault::memory_write &&
              stack_failure.context.cpu.registers.pc == third &&
              stack_failure.context.cpu.registers.sp == failing_stack &&
              stack_failure.context.cpu.execution == ExecutionState::running,
          "A zero-retirement fault after prior native blocks preserves the faulting descriptor");
  Harness middle_failure(true);
  const auto result = middle_failure.run();
  check(result, 4, {.pc = second + 2, .opcode = isa::opcode<isa::Opcode::staa_extended>()[0]});
  require(middle_failure.context.fault == Fault::memory_write &&
              middle_failure.context.address == failing_port &&
              middle_failure.context.cpu.registers.pc == second + 2 &&
              middle_failure.context.cpu.registers.d == 0x8000 &&
              middle_failure.context.cpu.registers.sp == stack &&
              middle_failure.ram[failing_port] == 0,
          "A partial block fault retains earlier progress and cannot execute its successor");
}
}

int main() {
  try {
    complete();
    boundaries();
    faults();
    sleep_boundaries();
    std::cout << "Native block dispatch, budgets, cold targets and precise faults passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
