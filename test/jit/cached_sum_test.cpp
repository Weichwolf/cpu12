// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address entry = 0x0200, input = 0x8000, accumulator = 0x6000;
constexpr Address other = 0x6100, stack = 0x7000, initial_d = 0xa55a;
constexpr Address default_seed = 0x7fff;
constexpr unsigned word_mask = 0xffff, byte_bits = 8, sign = 0x8000, input_words = 16;
constexpr unsigned carry = 1, overflow = 2, zero = 4, negative = 8, flags_nzv = 14;
constexpr Byte guard = 0xa5;
constexpr auto unlimited = std::numeric_limits<std::uint32_t>::max();
constexpr std::array<Byte, 2> sum_address{accumulator >> byte_bits, accumulator & 0xff};
constexpr std::array<Byte, 2> other_address{other >> byte_bits, other & 0xff};
constexpr std::array<Byte, 2> partial_address{accumulator >> byte_bits, 1}, one{0, 1};
constexpr std::array<Byte, 1> indexed_x_two{0x31}, branch_placeholder{0};
constexpr std::array<Byte, 2> counted_y_backwards{0x36, 0};

enum class Form : Byte {
  sum,
  extra_store,
  changed_d,
  overwritten_cache,
  prior_d,
  self_load,
  partial,
  constant
};
enum class Op : Byte {
  load_input,
  load_sum,
  add_sum,
  store_sum,
  store_other,
  store_y,
  add_one,
  partial,
  decrement,
  branch,
  counted,
  wait
};

struct Step {
  Op operation;
  Address pc;
  Byte opcode, length;
};

struct Program {
  std::vector<Byte> bytes;
  std::vector<Step> steps;
};

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

template <isa::Opcode opcode>
void append(Program &code, const Op operation, const std::span<const Byte> operands = {}) {
  constexpr auto encoding = isa::opcode<opcode>();
  code.steps.push_back({.operation = operation,
                        .pc = static_cast<Address>(entry + code.bytes.size()),
                        .opcode = encoding[0],
                        .length = static_cast<Byte>(encoding.size() + operands.size())});
  code.bytes.insert(code.bytes.end(), encoding.begin(), encoding.end());
  code.bytes.insert(code.bytes.end(), operands.begin(), operands.end());
}

Program program(const Form form, const bool counted) {
  Program code;
  if (form == Form::prior_d) {
    append<isa::Opcode::std_extended>(code, Op::store_other, other_address);
  }
  if (form == Form::self_load) {
    append<isa::Opcode::ldd_extended>(code, Op::load_sum, sum_address);
  } else {
    append<isa::Opcode::ldd_indexed>(code, Op::load_input, indexed_x_two);
  }
  append<isa::Opcode::addd_extended>(code, Op::add_sum, sum_address);
  if (form == Form::changed_d) { append<isa::Opcode::addd_immediate>(code, Op::add_one, one); }
  if (form == Form::extra_store || form == Form::constant) {
    append<isa::Opcode::std_extended>(code, Op::store_other, other_address);
  }
  if (form != Form::constant) {
    append<isa::Opcode::std_extended>(code, Op::store_sum, sum_address);
  }
  if (form == Form::overwritten_cache) {
    append<isa::Opcode::sty_extended>(code, Op::store_y, sum_address);
  }
  if (form == Form::partial) {
    append<isa::Opcode::staa_extended>(code, Op::partial, partial_address);
  }
  if (counted) {
    append<isa::Opcode::dbne_relative>(code, Op::counted, counted_y_backwards);
  } else {
    append<isa::Opcode::dey>(code, Op::decrement);
    append<isa::Opcode::bne_relative>(code, Op::branch, branch_placeholder);
  }
  code.bytes.back() = static_cast<Byte>(0x100U - code.bytes.size());
  append<isa::Opcode::wai>(code, Op::wait);
  return code;
}

struct Storage {
  std::array<Byte, (input_words * 2) + 2> input{};
  std::array<Byte, 4> sum{}, other{};
  std::array<Byte, 11> stack{};
  bool operator==(const Storage &) const = default;
};

template <class Image> auto &storage_byte(Image &ram, const bool alias, const Address address) {
  if (address >= accumulator && address < accumulator + 2) {
    const auto offset = std::size_t{address} - accumulator + 1;
    return alias ? ram.input.at(offset) : ram.sum.at(offset);
  }
  if (address >= input - 1 && address < input + input_words * 2 + 1) {
    return ram.input.at(std::size_t{address} - (input - 1U));
  }
  if (address >= other - 1 && address < other + 3) {
    return ram.other.at(std::size_t{address} - (other - 1U));
  }
  if (address >= stack && address < stack + ram.stack.size()) {
    return ram.stack.at(static_cast<std::size_t>(address - stack));
  }
  throw std::runtime_error("Independent oracle accessed an unmapped byte");
}

void word(Storage &ram, const bool alias, const Address address, const Address value) {
  storage_byte(ram, alias, address) = static_cast<Byte>(value >> byte_bits);
  storage_byte(ram, alias, static_cast<Address>(address + 1U)) = static_cast<Byte>(value);
}

Address word(const Storage &ram, const bool alias, const Address address) {
  return static_cast<Address>((unsigned{storage_byte(ram, alias, address)} << byte_bits) |
                              storage_byte(ram, alias, static_cast<Address>(address + 1U)));
}

Storage initial_storage(const Address seed, const bool alias) {
  Storage ram;
  ram.input.fill(guard);
  ram.sum.fill(guard);
  ram.other.fill(guard);
  ram.stack.fill(guard);
  constexpr std::array<Address, 8> values{0x8001, 0x7fff, 0xffff, 1, 0, 0x8000, 0x1234, 0xfedc};
  for (unsigned index = 0; index < input_words; ++index) {
    word(ram, alias, static_cast<Address>(input + (index * 2U)), values[index % values.size()]);
  }
  word(ram, alias, accumulator, seed);
  return ram;
}

struct Device {
  Program code;
  Storage ram;
  std::array<Ram, 5> regions;
  Core core;

  Device(const Form form, const bool counted, const bool alias)
      : code(program(form, counted)),
        regions{Ram{.first = entry, .bytes = code.bytes, .writable = false},
                Ram{.first = input - 1, .bytes = ram.input},
                Ram{.first = accumulator,
                    .bytes = alias ? std::span(ram.input).subspan(1, 2)
                                   : std::span(ram.sum).subspan(1, 2)},
                Ram{.first = other - 1, .bytes = ram.other},
                Ram{.first = stack, .bytes = ram.stack}},
        core(Memory::mapped(regions)) {}
};

void logical(Registers &state, const unsigned value, const unsigned sign_bit = sign) {
  state.ccr = static_cast<Byte>((state.ccr & ~flags_nzv) | (value == 0 ? zero : 0U) |
                                ((value & sign_bit) != 0 ? negative : 0U));
}

int signed_word(const Address value) {
  return value < sign ? static_cast<int>(value)
                      : static_cast<int>(value) - static_cast<int>(word_mask + 1U);
}

void add(Registers &state, const Address value) {
  const auto total = unsigned{state.d} + value;
  const auto signed_total = signed_word(state.d) + signed_word(value);
  const bool overflows = signed_total < std::numeric_limits<std::int16_t>::min() ||
                         signed_total > std::numeric_limits<std::int16_t>::max();
  state.d = static_cast<Address>(total);
  logical(state, state.d);
  state.ccr = static_cast<Byte>((state.ccr & ~carry) | (total > word_mask ? carry : 0U) |
                                (overflows ? overflow : 0U));
}

void wait(State &state, Storage &ram, const bool alias) {
  const auto sp = state.registers.sp;
  const auto &registers = state.registers;
  word(ram, alias, static_cast<Address>(sp - 2), registers.pc);
  word(ram, alias, static_cast<Address>(sp - 4), registers.y);
  word(ram, alias, static_cast<Address>(sp - 6), registers.x);
  storage_byte(ram, alias, static_cast<Address>(sp - 8)) = static_cast<Byte>(registers.d);
  storage_byte(ram, alias, static_cast<Address>(sp - 7)) =
      static_cast<Byte>(registers.d >> byte_bits);
  storage_byte(ram, alias, static_cast<Address>(sp - 9)) = registers.ccr;
  state.registers.sp = static_cast<Address>(sp - 9);
  state.execution = ExecutionState::waiting;
}

void perform(const Op operation, State &state, Storage &ram, const bool alias) {
  auto &registers = state.registers;
  switch (operation) {
    case Op::load_input:
      registers.d = word(ram, alias, registers.x);
      registers.x = static_cast<Address>(registers.x + 2U);
      logical(registers, registers.d);
      break;
    case Op::load_sum:
      registers.d = word(ram, alias, accumulator);
      logical(registers, registers.d);
      break;
    case Op::add_sum: add(registers, word(ram, alias, accumulator)); break;
    case Op::add_one: add(registers, 1); break;
    case Op::store_sum:
      word(ram, alias, accumulator, registers.d);
      logical(registers, registers.d);
      break;
    case Op::store_other:
      word(ram, alias, other, registers.d);
      logical(registers, registers.d);
      break;
    case Op::store_y:
      word(ram, alias, accumulator, registers.y);
      logical(registers, registers.y);
      break;
    case Op::partial:
      storage_byte(ram, alias, accumulator + 1) = static_cast<Byte>(registers.d >> byte_bits);
      logical(registers, registers.d >> byte_bits, 0x80);
      break;
    case Op::decrement:
      registers.y = static_cast<Address>((unsigned{registers.y} + word_mask) & word_mask);
      registers.ccr = static_cast<Byte>((registers.ccr & ~zero) | (registers.y == 0 ? zero : 0U));
      break;
    case Op::branch:
      if ((registers.ccr & zero) == 0) { registers.pc = entry; }
      break;
    case Op::counted:
      registers.y = static_cast<Address>((unsigned{registers.y} + word_mask) & word_mask);
      if (registers.y != 0) { registers.pc = entry; }
      break;
    case Op::wait: wait(state, ram, alias); break;
    default: throw std::logic_error("Unknown independent oracle operation");
  }
}

RunResult advance(
    const Program &code, State &state, Storage &ram, const bool alias, const std::uint32_t budget) {
  RunResult result{.state = state.execution, .pc = state.registers.pc};
  while (result.retired < budget && state.execution == ExecutionState::running) {
    const auto step = std::ranges::find(code.steps, state.registers.pc, &Step::pc);
    require(step != code.steps.end(), "Independent oracle escaped its authored program");
    result.pc = step->pc;
    result.opcode = step->opcode;
    state.registers.pc = static_cast<Address>(state.registers.pc + step->length);
    perform(step->operation, state, ram, alias);
    ++result.retired;
  }
  result.state = state.execution;
  return result;
}

struct Point {
  Address seed = default_seed, count = 3;
  Byte flags = 0;
};

unsigned check(Device &device, const Point point, const bool alias, const unsigned budget) {
  device.ram = initial_storage(point.seed, alias);
  auto wanted_ram = device.ram;
  State wanted{};
  wanted.registers = {.pc = entry,
                      .d = initial_d,
                      .x = input,
                      .y = point.count,
                      .sp = stack + 10,
                      .ccr = point.flags};
  device.core.set_registers(wanted.registers);
  const auto expected = advance(device.code, wanted, wanted_ram, alias, budget);
  const auto actual = device.core.run(budget);
  require(actual == expected && device.core.state() == wanted && device.ram == wanted_ram,
          "Cached reduction differs at an independent instruction deadline");
  if (wanted.execution != ExecutionState::running) { return 1; }
  constexpr Address external_change = 0x5aa5;
  const auto replacement = static_cast<Address>(point.seed ^ external_change);
  word(device.ram, alias, accumulator, replacement);
  word(wanted_ram, alias, accumulator, replacement);
  const auto resumed = advance(device.code, wanted, wanted_ram, alias, unlimited);
  require(device.core.run(unlimited) == resumed && device.core.state() == wanted &&
              device.ram == wanted_ram,
          "Resumed reduction must reload consumer changes and retain exact registers/flags/RAM");
  return 2;
}

unsigned point(Device &device, const Point input_point, const bool alias) {
  unsigned checked = 0;
  const auto total = (input_point.count * (device.code.steps.size() - 1U)) + 1U;
  for (unsigned budget = 0; budget <= total; ++budget) {
    checked += check(device, input_point, alias, budget);
  }
  return checked;
}

unsigned cases(const Form form, const bool counted, const bool alias) {
  Device device(form, counted, alias);
  unsigned checked = point(device, {}, alias);
  const auto compiled = device.core.statistics().compiled_entries;
  for (unsigned flags = 0; flags <= std::numeric_limits<Byte>::max(); ++flags) {
    checked += point(device, {.flags = static_cast<Byte>(flags)}, alias);
  }
  constexpr std::array<Address, 4> seeds{0, 0x7fff, 0x8000, 0xffff};
  constexpr std::array<Byte, 8> flags{0, 1, 0x0f, 0x20, 0x40, 0x80, 0xfe, 0xff};
  for (const auto seed : seeds) {
    for (const auto count : {1, 3}) {
      for (const auto ccr : flags) {
        checked += point(
            device, {.seed = seed, .count = static_cast<Address>(count), .flags = ccr}, alias);
      }
    }
  }
  require(device.core.statistics().compiled_entries == compiled,
          "All reduction entries must remain retained across values, flags and deadlines");
  require(device.code.bytes == program(form, counted).bytes,
          "Execution must preserve all ROM bytes");
  return checked;
}
}

int main() {
  try {
    unsigned checked = 0;
    for (const auto form : {Form::sum,
                            Form::extra_store,
                            Form::changed_d,
                            Form::overwritten_cache,
                            Form::prior_d,
                            Form::self_load,
                            Form::partial,
                            Form::constant}) {
      for (const auto counted : {false, true}) {
        for (const auto alias : {false, true}) { checked += cases(form, counted, alias); }
      }
    }
    std::cout << checked
              << " independent reduction, decline, alias, deadline and resume cases passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
