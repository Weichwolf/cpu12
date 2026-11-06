// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "observed_core.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
using Image = std::array<Byte, address_bytes>;
constexpr Address entry = 0x0200, input = 0x4000, accumulator = 0x6000, stack = 0x9000;
constexpr Address initial_d = 0x1234;
constexpr Address add_pc = entry + 2, store_pc = entry + 5, loop_pc = entry + 8;
constexpr Address stop_pc = entry + 11, return_pc = stop_pc + isa::stop.size();
constexpr unsigned byte_bits = 8, byte_mask = 0xff, word_mask = 0xffff;
constexpr unsigned sign = 0x8000, overflow = 2, negative = 8, zero = 4;
constexpr unsigned retained_nzv = 0xf1, retained_nzvc = 0xf0;
constexpr Byte guard = 0xa5;
// X postincrement by two; DBNE Y uses a negative nine-bit displacement.
constexpr std::array<Byte, 1> increment_x{0x31}, decrement_y{0x36}, backedge{0xf5};
constexpr std::array<Byte, 2> sum_operand{accumulator >> byte_bits, accumulator &byte_mask};
constexpr auto program = isa::program(isa::opcode<isa::Opcode::ldd_indexed>(),
                                      increment_x,
                                      isa::opcode<isa::Opcode::addd_extended>(),
                                      sum_operand,
                                      isa::opcode<isa::Opcode::std_extended>(),
                                      sum_operand,
                                      isa::opcode<isa::Opcode::dbne_relative>(),
                                      decrement_y,
                                      backedge,
                                      isa::stop);
std::uint64_t checked = 0;

void word(Image &ram, const Address address, const Address value) {
  ram[address] = static_cast<Byte>(value >> byte_bits);
  ram[address + 1U] = static_cast<Byte>(value & byte_mask);
}

unsigned word(const Image &ram, const Address address) {
  return (unsigned{ram[address]} << byte_bits) | ram[address + 1U];
}

Byte nz(const unsigned value) {
  return static_cast<Byte>((value >= sign ? negative : 0U) | (value == 0 ? zero : 0U));
}

struct Model {
  Image ram;
  Registers registers;
  ExecutionState execution = ExecutionState::running;
  Address last_pc = entry;
};

void external(Image &ram, const Registers registers, const unsigned epoch, const unsigned seed) {
  constexpr unsigned stride = 257;
  const auto value = static_cast<Address>((epoch * stride + seed + sign) & word_mask);
  word(ram, accumulator, value);
  word(ram, registers.x, static_cast<Address>(value ^ word_mask));
  // A past input may change after its LDD, before ADDD or flag publication.
  if (registers.x > input) {
    word(ram, static_cast<Address>(registers.x - 2U), static_cast<Address>(value ^ sign));
  }
}

void add(Model &model) {
  const auto left = unsigned{model.registers.d};
  const auto right = word(model.ram, accumulator);
  const auto wide = left + right;
  const auto value = wide & word_mask;
  const auto signed_left = left < sign ? static_cast<int>(left) : static_cast<int>(left) - 65536;
  const auto signed_right =
      right < sign ? static_cast<int>(right) : static_cast<int>(right) - 65536;
  const auto signed_sum = signed_left + signed_right;
  const unsigned flags =
      static_cast<unsigned>(wide > word_mask) |
      (signed_sum < -static_cast<int>(sign) || signed_sum >= static_cast<int>(sign) ? overflow
                                                                                    : 0U);
  model.registers.d = static_cast<Address>(value);
  model.registers.ccr =
      static_cast<Byte>((model.registers.ccr & retained_nzvc) | flags | nz(value));
  model.registers.pc = store_pc;
}

void stop(Model &model) {
  const auto registers = model.registers;
  const std::array<Byte, 9> frame{registers.ccr,
                                  static_cast<Byte>(registers.d & byte_mask),
                                  static_cast<Byte>(registers.d >> byte_bits),
                                  static_cast<Byte>(registers.x >> byte_bits),
                                  static_cast<Byte>(registers.x & byte_mask),
                                  static_cast<Byte>(registers.y >> byte_bits),
                                  static_cast<Byte>(registers.y & byte_mask),
                                  static_cast<Byte>(return_pc >> byte_bits),
                                  static_cast<Byte>(return_pc & byte_mask)};
  std::ranges::copy(frame, model.ram.begin() + registers.sp - frame.size());
  model.registers.pc = return_pc;
  model.registers.sp = static_cast<Address>(registers.sp - frame.size());
  model.execution = ExecutionState::stopped;
}

void step(Model &model) {
  auto &registers = model.registers;
  model.last_pc = registers.pc;
  if (registers.pc == entry) {
    registers.d = static_cast<Address>(word(model.ram, registers.x));
    registers.x = static_cast<Address>(registers.x + 2U);
    registers.ccr = static_cast<Byte>((registers.ccr & retained_nzv) | nz(registers.d));
    registers.pc = add_pc;
  } else if (registers.pc == add_pc) {
    add(model);
  } else if (registers.pc == store_pc) {
    word(model.ram, accumulator, registers.d);
    registers.ccr = static_cast<Byte>((registers.ccr & retained_nzv) | nz(registers.d));
    registers.pc = loop_pc;
  } else if (registers.pc == loop_pc) {
    --registers.y;
    registers.pc = registers.y == 0 ? stop_pc : entry;
  } else if (registers.pc == stop_pc) {
    stop(model);
  } else {
    throw std::runtime_error("Unexpected independent stream-model PC");
  }
}

struct Schedule {
  unsigned cap, seed;
};

void run(ObservedCore &core, Image &ram, Model &model, const Schedule schedule) {
  const auto expected_instructions = (unsigned{model.registers.y} * 4U) + 1U;
  const auto before = core.statistics().native_instructions;
  core.set_registers(model.registers);
  unsigned retired = 0;
  unsigned epoch = 0;
  while (retired < expected_instructions) {
    external(ram, model.registers, epoch, schedule.seed);
    external(model.ram, model.registers, epoch, schedule.seed);
    const auto grant = std::min(schedule.cap, expected_instructions - retired);
    for (unsigned index = 0; index < grant; ++index) { step(model); }
    const auto actual = core.run(grant);
    const auto opcode = program[model.last_pc - entry];
    const auto observed = core.registers();
    if (actual.fault != Fault::none || actual.retired != grant || actual.state != model.execution ||
        actual.pc != model.last_pc || actual.opcode != opcode || observed != model.registers ||
        ram != model.ram || core.state().failure != RunResult{} || core.state().irq_delay != 0 ||
        core.state().returned_frame) {
      std::cerr << "cap=" << schedule.cap << " seed=" << schedule.seed << " epoch=" << epoch
                << " grant=" << grant << " retired=" << actual.retired
                << " fault=" << static_cast<unsigned>(actual.fault) << " pc=" << observed.pc << '/'
                << model.registers.pc << " d=" << observed.d << '/' << model.registers.d
                << " x=" << observed.x << '/' << model.registers.x << " y=" << observed.y << '/'
                << model.registers.y << " ccr=" << unsigned{observed.ccr} << '/'
                << unsigned{model.registers.ccr} << '\n';
      throw std::runtime_error("Bounded stream differs from independent interleaved RAM/state");
    }
    retired += grant;
    ++epoch;
    ++checked;
  }
  const auto statistics = core.statistics();
  if (statistics.native_instructions - before != expected_instructions ||
      statistics.read_callbacks != 0 || statistics.write_callbacks != 0) {
    throw std::runtime_error("Bounded stream retirement or ordinary-RAM access accounting differs");
  }
}

void exercise() {
  Image ram;
  ram.fill(guard);
  std::ranges::copy(program, ram.begin() + entry);
  const auto original = ram;
  const auto end = static_cast<Address>(entry + program.size());
  const std::array regions{Ram{.bytes = std::span(ram).first(entry)},
                           Ram{.first = entry,
                               .bytes = std::span(ram).subspan(entry, program.size()),
                               .writable = false},
                           Ram{.first = end, .bytes = std::span(ram).subspan(end)}};
  ObservedCore core(Memory::mapped(regions));
  constexpr std::array<Address, 3> counts{1, 2, 7};
  constexpr std::array<unsigned, 7> caps{1, 2, 3, 4, 5, 7, 16};
  std::uint64_t compiled = 0;
  for (const auto count : counts) {
    for (const auto cap : caps) {
      for (unsigned ccr = 0; ccr < 0x80U; ++ccr) {
        ram = original;
        Model model{.ram = original,
                    .registers = {.pc = entry,
                                  .d = initial_d,
                                  .x = input,
                                  .y = count,
                                  .sp = stack,
                                  .ccr = static_cast<Byte>(ccr)}};
        run(core, ram, model, {.cap = cap, .seed = ccr});
        if (compiled == 0) { compiled = core.statistics().compiled_entries; }
        if (core.statistics().compiled_entries != compiled) {
          throw std::runtime_error("Changing stream grants or RAM must reuse warmed native code");
        }
      }
    }
  }
}
}

int main() {
  try {
    exercise();
    std::cout << checked << " bounded stream boundaries with live external RAM events passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
