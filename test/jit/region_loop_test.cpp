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
constexpr Address entry = 0x0200;
constexpr unsigned retained_flags = 0xf0;
constexpr Registers seed_registers{
    .pc = entry, .d = 0x7ffd, .x = 0x3456, .y = 0x5678, .sp = 0x9000, .ccr = 0xa5};
constexpr unsigned word_values = 65536, word_mask = word_values - 1, sign = 0x8000;
constexpr std::array<Byte, 2> addend{0, 3}, absolute_target{2, 0};
constexpr std::array<Byte, 1> skip_relative{2}, skip_absolute{3}, relative_back{0xf8};
constexpr std::array<Byte, 2> skip_wide{0, 4}, wide_back{0xff, 0xf4};
constexpr auto relative = isa::program(isa::opcode<isa::Opcode::addd_immediate>(),
                                       addend,
                                       isa::dex,
                                       isa::opcode<isa::Opcode::beq_relative>(),
                                       skip_relative,
                                       isa::opcode<isa::Opcode::bra_relative>(),
                                       relative_back,
                                       isa::stop);
constexpr auto absolute = isa::program(isa::opcode<isa::Opcode::addd_immediate>(),
                                       addend,
                                       isa::iny,
                                       isa::opcode<isa::Opcode::beq_relative>(),
                                       skip_absolute,
                                       isa::opcode<isa::Opcode::jmp_extended>(),
                                       absolute_target,
                                       isa::stop);
constexpr auto wide = isa::program(isa::opcode<isa::Opcode::addd_immediate>(),
                                   addend,
                                   isa::dey,
                                   isa::opcode<isa::Opcode::lbeq_relative>(),
                                   skip_wide,
                                   isa::opcode<isa::Opcode::lbra_relative>(),
                                   wide_back,
                                   isa::stop);

struct Form {
  std::span<const Byte> program;
  unsigned branch_bytes;
  Byte counter_opcode, exit_opcode, backedge_opcode;
  bool y = false, increment = false;
};

constexpr std::array forms{Form{.program = relative,
                                .branch_bytes = 2,
                                .counter_opcode = isa::dex[0],
                                .exit_opcode = isa::opcode<isa::Opcode::beq_relative>()[0],
                                .backedge_opcode = isa::opcode<isa::Opcode::bra_relative>()[0]},
                           Form{.program = absolute,
                                .branch_bytes = 2,
                                .counter_opcode = isa::iny[0],
                                .exit_opcode = isa::opcode<isa::Opcode::beq_relative>()[0],
                                .backedge_opcode = isa::opcode<isa::Opcode::jmp_extended>()[0],
                                .y = true,
                                .increment = true},
                           Form{.program = wide,
                                .branch_bytes = 4,
                                .counter_opcode = isa::dey[0],
                                .exit_opcode = isa::opcode<isa::Opcode::lbeq_relative>()[0],
                                .backedge_opcode = isa::opcode<isa::Opcode::lbra_relative>()[0],
                                .y = true}};

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Device {
  std::array<Byte, address_bytes> ram{};
  std::array<Ram, 3> regions;
  Core core;

  explicit Device(const Form &form)
      : regions{Ram{.bytes = std::span(ram).first(entry)},
                Ram{.first = entry,
                    .bytes = std::span(ram).subspan(entry, form.program.size()),
                    .writable = false},
                Ram{.first = static_cast<Address>(entry + form.program.size()),
                    .bytes = std::span(ram).subspan(entry + form.program.size())}},
        core(Memory::mapped(regions)) {
    std::ranges::copy(form.program, ram.begin() + entry);
  }
};

unsigned trips(const Form &form, const Registers initial) {
  const auto value = form.y ? initial.y : initial.x;
  const auto count = form.increment ? (word_values - value) % word_values : value;
  return count == 0 ? word_values : count;
}

Registers expected(const Form &form, Registers initial, const unsigned budget) {
  const auto whole = budget / 4;
  const auto part = budget % 4;
  const auto additions = whole + static_cast<unsigned>(part != 0);
  const auto steps = whole + static_cast<unsigned>(part >= 2);
  if (additions != 0) {
    const auto previous = (unsigned{initial.d} + (additions - 1) * 3) & word_mask;
    const auto sum = previous + 3;
    const auto value = sum & word_mask;
    const unsigned zero = value == 0 ? 4U : 0U;
    const unsigned negative = value >= sign ? 8U : 0U;
    const unsigned overflow = previous < sign && sum >= sign ? 2U : 0U;
    const unsigned carry = sum >= word_values ? 1U : 0U;
    initial.d = static_cast<Address>(value);
    initial.ccr =
        static_cast<Byte>((initial.ccr & retained_flags) | zero | negative | overflow | carry);
  }
  auto &counter = form.y ? initial.y : initial.x;
  counter = static_cast<Address>(form.increment ? unsigned{counter} + steps
                                                : unsigned{counter} + word_values - steps);
  if (budget != 0 && part != 1) {
    initial.ccr = static_cast<Byte>((initial.ccr & ~4U) | (counter == 0 ? 4U : 0U));
  }
  if (part == 1) { initial.pc = entry + 3; }
  if (part == 2) { initial.pc = entry + 4; }
  if (part == 3) {
    initial.pc = static_cast<Address>(
        entry + (counter == 0 ? form.program.size() - isa::stop.size() : 4 + form.branch_bytes));
  }
  return initial;
}

void check(Device &device, const Form &form, const Registers initial, const unsigned budget) {
  const auto ram = device.ram;
  device.core.set_registers(initial);
  const auto result = device.core.run(budget);
  require(result.retired == budget && result.fault == Fault::none &&
              result.state == ExecutionState::running &&
              device.core.registers() == expected(form, initial, budget) && device.ram == ram,
          "Threaded loop differs from independent bounded arithmetic, flags, PC or memory");
  if (budget == 0) { return; }
  constexpr auto add_opcode = isa::opcode<isa::Opcode::addd_immediate>()[0];
  const std::array opcodes{form.backedge_opcode, add_opcode, form.counter_opcode, form.exit_opcode};
  const std::array addresses{static_cast<Address>(entry + 4 + form.branch_bytes),
                             entry,
                             static_cast<Address>(entry + 3),
                             static_cast<Address>(entry + 4)};
  require(result.pc == addresses[budget % 4] && result.opcode == opcodes[budget % 4],
          "A bounded loop must identify the actual last retired original guest instruction");
}

Registers seed(const Form &form, const Address count) {
  auto result = seed_registers;
  (form.y ? result.y : result.x) = count;
  return result;
}

void resumed_cases(Device &device, const Form &form) {
  const auto initial = seed(form, form.increment ? word_values - 7 : 7);
  const auto total = (trips(form, initial) * 4) - 1;
  constexpr std::array prefixes{1U, 2U, 3U, 4U, 5U, 6U, 7U};
  for (const auto prefix : prefixes) {
    check(device, form, initial, prefix);
    const auto result = device.core.run(total - prefix);
    require(
        result.fault == Fault::none && result.retired == total - prefix &&
            result.state == ExecutionState::running &&
            device.core.registers() == expected(form, initial, total) && result.pc == entry + 4 &&
            result.opcode == form.exit_opcode,
        "Execution resumed inside either block must preserve the original final state and exit");
  }
}

void mutable_backedge() {
  const auto &form = forms.front();
  Device device(form);
  constexpr unsigned body_bytes = 6;
  const std::array grants{
      Ram{.bytes = std::span(device.ram).first(entry)},
      Ram{.first = entry,
          .bytes = std::span(device.ram).subspan(entry, body_bytes),
          .writable = false},
      Ram{.first = entry + body_bytes, .bytes = std::span(device.ram).subspan(entry + body_bytes)}};
  device.core.set_memory(Memory::mapped(grants));
  auto initial = seed(form, 3);
  initial.d = 0;
  check(device, form, initial, (3 * 4) - 1);
  require(device.core.statistics().compiled_entries > 1,
          "A writable backedge must remain outside the immutable connected region");
  // Change BRA's target from ADDD to DEX: the sum must now be added only once.
  constexpr Byte target_counter = 0xfb;
  device.ram[entry + body_bytes + 1] = target_counter;
  device.core.set_registers(initial);
  const auto result = device.core.run(9);
  auto answer = initial;
  answer.d = 3;
  answer.x = 0;
  answer.ccr = static_cast<Byte>((initial.ccr & retained_flags) | 4U);
  answer.pc = entry + form.program.size() - isa::stop.size();
  require(result.fault == Fault::none && result.retired == 9 &&
              result.state == ExecutionState::running && device.core.registers() == answer &&
              result.pc == entry + 4 && result.opcode == form.exit_opcode,
          "A changed writable backedge must execute its current target without stale fusion");
}

void check_counted(Device &device, const Form &form, const Registers initial) {
  const auto ram = device.ram;
  const auto iterations = trips(form, initial);
  const auto previous = (unsigned{initial.d} + (iterations - 1) * 3) & word_mask;
  const auto sum = previous + 3;
  auto answer = initial;
  answer.d = static_cast<Address>(sum);
  (form.y ? answer.y : answer.x) = 0;
  answer.pc = entry + 6;
  answer.ccr = static_cast<Byte>(
      (initial.ccr & retained_flags) | ((sum & word_mask) == 0 ? 4U : 0U) |
      ((sum & word_mask) >= sign ? 8U : 0U) | (previous < sign && sum >= sign ? 2U : 0U) |
      (sum >= word_values ? 1U : 0U));
  for (const unsigned prefix : {0U, 1U}) {
    device.core.set_registers(initial);
    if (prefix != 0) {
      const auto first = device.core.run(prefix);
      require(first.retired == prefix && first.fault == Fault::none,
              "A partial counted loop must retire its actual first instruction");
    }
    const auto budget = (iterations * 2) - prefix;
    const auto result = device.core.run(budget);
    require(result.retired == budget && result.fault == Fault::none &&
                result.state == ExecutionState::running && result.pc == entry + 3 &&
                result.opcode == isa::opcode<isa::Opcode::dbne_relative>()[0] &&
                device.core.registers() == answer && device.ram == ram,
            "DBNE/IBNE must preserve ADD flags, counter wrap and resumed branch semantics");
  }
}

void counted_cases() {
  constexpr std::array<Byte, 2> decrement_x{0x35, 0xfa};
  constexpr std::array<Byte, 2> increment_y{0xb6, 0xfa};
  constexpr auto down = isa::program(isa::opcode<isa::Opcode::addd_immediate>(),
                                     addend,
                                     isa::opcode<isa::Opcode::dbne_relative>(),
                                     decrement_x,
                                     isa::stop);
  constexpr auto up = isa::program(isa::opcode<isa::Opcode::addd_immediate>(),
                                   addend,
                                   isa::opcode<isa::Opcode::ibne_relative>(),
                                   increment_y,
                                   isa::stop);
  const std::array<std::span<const Byte>, 2> programs{down, up};
  constexpr std::array<Address, 5> counts{1, 2, 128, 0, 0xffff};
  constexpr std::array<Address, 4> values{0, 0x7ffd, 0x8000, 0xffff};
  for (unsigned direction = 0; direction < programs.size(); ++direction) {
    const Form form{.program = programs[direction],
                    .branch_bytes = 0,
                    .counter_opcode = 0,
                    .exit_opcode = 0,
                    .backedge_opcode = 0,
                    .y = direction != 0,
                    .increment = direction != 0};
    Device device(form);
    for (const auto count : counts) {
      for (const auto value : values) {
        auto initial = seed(form, count);
        initial.d = value;
        check_counted(device, form, initial);
      }
    }
  }
}

void bounded_cases(const Form &form) {
  Device device(form);
  auto initial = seed(form, form.increment ? word_values - 32 : 32);
  check(device, form, initial, (32 * 4) - 1);
  require(device.core.statistics().compiled_entries == 1,
          "Connected counted blocks must execute as one retained native loop");
  constexpr std::array<Address, 4> values{0, 0x7ffd, 0x8000, 0xffff};
  constexpr std::array<Byte, 3> flags{0, 0x25, 0xff};
  constexpr std::array<Address, 4> counts{1, 2, 7, 128};
  for (const auto count : counts) {
    initial = seed(form, form.increment ? static_cast<Address>(word_values - count) : count);
    const auto total = (trips(form, initial) * 4) - 1;
    const std::array budgets{0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, total - 1, total};
    for (const auto value : values) {
      for (const auto ccr : flags) {
        initial.d = value;
        initial.ccr = ccr;
        for (const auto budget : budgets) {
          if (budget <= total) { check(device, form, initial, budget); }
        }
      }
    }
  }
  resumed_cases(device, form);
  constexpr std::array<Address, 2> wraps{0, 0xffff};
  for (const auto count : wraps) {
    initial = seed(form, count);
    const auto total = (trips(form, initial) * 4) - 1;
    check(device, form, initial, total - 1);
    check(device, form, initial, total);
    const auto compiled = device.core.statistics().compiled_entries;
    check(device, form, initial, total);
    require(device.core.statistics().compiled_entries == compiled,
            "Warm full loops reuse immutable translations without budget-specific compilation");
  }
}
}

int main() {
  try {
    for (const auto &form : forms) { bounded_cases(form); }
    mutable_backedge();
    counted_cases();
    std::cout << "Connected counted loops preserve branch weights, flags, wrap and exact budgets\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
