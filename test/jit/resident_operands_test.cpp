// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr Address entry = 0x0200, operand = 0x4000, alias = 0x6000;
constexpr unsigned byte_bits = 8, byte_mask = 0xff, word_mask = 0xffff;
constexpr unsigned negative = 8, zero = 4, overflow = 2, half = 0x20;
constexpr Byte guard = 0xa5, known_flag = 0x80;
constexpr Registers seed{.pc = entry, .y = 0x5a5a, .sp = 0x7fff};
std::uint64_t checked = 0;

enum class Function : Byte { add, subtract, compare, bit_and, bit_or, bit_xor, bit_test };
enum class Store : Byte { none, accumulator, word };

struct Form {
  isa::Opcode opcode;
  Function function;
  bool word = false, b = false;
};

constexpr std::array forms{
    Form{.opcode = isa::Opcode::adda_extended, .function = Function::add},
    Form{.opcode = isa::Opcode::addb_extended, .function = Function::add, .b = true},
    Form{.opcode = isa::Opcode::suba_extended, .function = Function::subtract},
    Form{.opcode = isa::Opcode::subb_extended, .function = Function::subtract, .b = true},
    Form{.opcode = isa::Opcode::cmpa_extended, .function = Function::compare},
    Form{.opcode = isa::Opcode::cmpb_extended, .function = Function::compare, .b = true},
    Form{.opcode = isa::Opcode::anda_extended, .function = Function::bit_and},
    Form{.opcode = isa::Opcode::andb_extended, .function = Function::bit_and, .b = true},
    Form{.opcode = isa::Opcode::oraa_extended, .function = Function::bit_or},
    Form{.opcode = isa::Opcode::orab_extended, .function = Function::bit_or, .b = true},
    Form{.opcode = isa::Opcode::eora_extended, .function = Function::bit_xor},
    Form{.opcode = isa::Opcode::eorb_extended, .function = Function::bit_xor, .b = true},
    Form{.opcode = isa::Opcode::bita_extended, .function = Function::bit_test},
    Form{.opcode = isa::Opcode::bitb_extended, .function = Function::bit_test, .b = true},
    Form{.opcode = isa::Opcode::addd_extended, .function = Function::add, .word = true},
    Form{.opcode = isa::Opcode::subd_extended, .function = Function::subtract, .word = true},
    Form{.opcode = isa::Opcode::cpd_extended, .function = Function::compare, .word = true}};

struct Program {
  std::array<Byte, 9> bytes;
  std::size_t size;
};

Program program(const Form form, const Store store) {
  constexpr Byte decrement_x_backward = 0x35;
  constexpr Byte branch = static_cast<Byte>(isa::Opcode::dbne_relative);
  auto store_opcode = isa::Opcode::std_extended;
  if (!form.word && store != Store::word) {
    store_opcode = form.b ? isa::Opcode::stab_extended : isa::Opcode::staa_extended;
  }
  Program result{.bytes = {static_cast<Byte>(form.opcode),
                           operand >> byte_bits,
                           operand & byte_mask,
                           static_cast<Byte>(store_opcode),
                           alias >> byte_bits,
                           alias & byte_mask,
                           branch,
                           decrement_x_backward,
                           static_cast<Byte>(0x100U - 9U)},
                 .size = 9};
  if (store == Store::none) {
    result.bytes[3] = branch;
    result.bytes[4] = decrement_x_backward;
    result.bytes[5] = static_cast<Byte>(0x100U - 6U);
    result.size = 6;
  }
  return result;
}

struct Storage {
  std::array<Byte, 4> bytes{guard, 0, 0, guard};
  std::array<Byte, 2> known{known_flag, known_flag};
};

struct Device {
  Program code;
  Storage storage;
  std::array<Ram, 3> regions;
  Core core;

  Device(const Form form, const Store store, const bool knowledge)
      : code(program(form, store)),
        regions{
            Ram{.first = entry, .bytes = std::span(code.bytes).first(code.size), .writable = false},
            Ram{.first = operand,
                .bytes = std::span(storage.bytes).subspan(1, 2),
                .known = knowledge ? std::span(storage.known) : std::span<Byte>{},
                .writable = false},
            Ram{.first = alias, .bytes = std::span(storage.bytes).subspan(1, 2)}},
        core(Memory::mapped(regions)) {}
};

struct Input {
  Address d, memory, count;
  Byte ccr;
};

struct Expected {
  Registers registers;
  std::array<Byte, 4> bytes;
  Address last_pc = entry;
  Byte opcode = 0;
};

Byte nz(const unsigned value, const unsigned sign) {
  return static_cast<Byte>((value == 0 ? zero : 0U) | (value >= sign ? negative : 0U));
}

int signed_value(const unsigned value, const unsigned sign) {
  return value < sign ? static_cast<int>(value)
                      : static_cast<int>(value) - static_cast<int>(sign * 2U);
}

unsigned accumulator(const Registers state, const Form form) {
  if (form.word) { return state.d; }
  return form.b ? state.d & byte_mask : state.d >> byte_bits;
}

struct Math {
  unsigned left, right;
  unsigned value = 0, flags = 0, changed = 15;
};

void numeric(Math &result, const Form form) {
  const auto mask = form.word ? word_mask : byte_mask;
  const auto sign = (mask + 1U) / 2U;
  const bool add = form.function == Function::add;
  result.value = add ? result.left + result.right : result.left + (mask + 1U) - result.right;
  result.flags = static_cast<unsigned>(add ? result.value > mask : result.left < result.right);
  const auto signed_left = signed_value(result.left, sign);
  const auto signed_right = signed_value(result.right, sign);
  const auto wide = add ? signed_left + signed_right : signed_left - signed_right;
  if (wide < -static_cast<int>(sign) || wide >= static_cast<int>(sign)) {
    result.flags |= overflow;
  }
  if (add && !form.word) {
    result.changed |= half;
    if ((result.left % 16U) + (result.right % 16U) >= 16U) { result.flags |= half; }
  }
}

void arithmetic(Expected &state, const Form form) {
  const auto mask = form.word ? word_mask : byte_mask;
  const auto sign = (mask + 1U) / 2U;
  Math result{.left = accumulator(state.registers, form),
              .right = form.word ? (unsigned{state.bytes[1]} << byte_bits) | state.bytes[2]
                                 : unsigned{state.bytes[1]}};
  if (form.function == Function::add || form.function == Function::subtract ||
      form.function == Function::compare) {
    numeric(result, form);
  } else {
    result.changed = 14;
    if (form.function == Function::bit_and || form.function == Function::bit_test) {
      result.value = result.left & result.right;
    } else if (form.function == Function::bit_or) {
      result.value = result.left | result.right;
    } else {
      result.value = result.left ^ result.right;
    }
  }
  result.value &= mask;
  state.registers.ccr = static_cast<Byte>((state.registers.ccr & ~result.changed) | result.flags |
                                          nz(result.value, sign));
  if (form.function == Function::compare || form.function == Function::bit_test) { return; }
  if (form.word) {
    state.registers.d = static_cast<Address>(result.value);
  } else {
    state.registers.d = static_cast<Address>(
        form.b ? (state.registers.d & ~byte_mask) | result.value
               : (result.value << byte_bits) | (state.registers.d & byte_mask));
  }
}

void write(Expected &state, const Form form, const Store store) {
  const bool word = form.word || store == Store::word;
  auto width = form;
  width.word = word;
  const auto value = accumulator(state.registers, width);
  state.bytes[1] = static_cast<Byte>(word ? value >> byte_bits : value);
  if (word) { state.bytes[2] = static_cast<Byte>(value & byte_mask); }
  constexpr unsigned retained = 0xf1;
  state.registers.ccr =
      static_cast<Byte>((state.registers.ccr & retained) | nz(value, word ? 0x8000U : 0x80U));
}

Expected predict(const Device &device, const Form form, const Store store, const unsigned budget) {
  Expected state{.registers = device.core.registers(), .bytes = device.storage.bytes};
  const unsigned body_size = store == Store::none ? 2U : 3U;
  for (unsigned step = 0; step < budget; ++step) {
    const auto phase = step % body_size;
    state.last_pc = state.registers.pc;
    state.opcode = device.code.bytes[std::size_t{phase} * 3U];
    state.registers.pc = static_cast<Address>(state.registers.pc + 3U);
    if (phase == 0) {
      arithmetic(state, form);
    } else if (phase == body_size - 1U) {
      state.registers.x = static_cast<Address>(state.registers.x - 1U);
      if (state.registers.x != 0) { state.registers.pc = entry; }
    } else {
      write(state, form, store);
    }
  }
  return state;
}

void one(
    Device &device, const Form form, const Store store, const Input input, const unsigned budget) {
  device.storage.bytes = {guard,
                          static_cast<Byte>(input.memory >> byte_bits),
                          static_cast<Byte>(input.memory & byte_mask),
                          guard};
  auto initial = seed;
  initial.d = input.d;
  initial.x = input.count;
  initial.ccr = input.ccr;
  device.core.set_registers(initial);
  const auto expected = predict(device, form, store, budget);
  const auto actual = device.core.run(budget);
  if (actual.fault != Fault::none || actual.state != ExecutionState::running ||
      actual.retired != budget || actual.pc != expected.last_pc ||
      actual.opcode != expected.opcode || device.core.registers() != expected.registers ||
      device.storage.bytes != expected.bytes ||
      device.storage.known != std::array<Byte, 2>{known_flag, known_flag}) {
    throw std::runtime_error(
        "Resident operand differs from independent live-memory/CCR expectation");
  }
  ++checked;
}

void exercise(const Form form, const Store store, const bool knowledge) {
  Device device(form, store, knowledge);
  constexpr std::array<Address, 8> d{0, 0x017f, 0x7fff, 0x8000, 0xff80, 0xffff, 0x0f7f, 0x807f};
  constexpr std::array<Address, 8> memory{
      0xffff, 0x7fff, 1, 0x8000, 0x0100, 0xff00, 0x0101, 0x8080};
  constexpr std::array<Address, 3> counts{1, 2, 7};
  for (std::size_t index = 0; index < d.size(); ++index) {
    for (const auto count : counts) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        const Input input{
            .d = d[index], .memory = memory[index], .count = count, .ccr = static_cast<Byte>(ccr)};
        const auto instructions = count * (store == Store::none ? 2U : 3U);
        one(device, form, store, input, instructions);
        if (ccr != 0 && ccr != byte_mask) { continue; }
        for (unsigned budget = 1; budget < instructions; ++budget) {
          one(device, form, store, input, budget);
        }
      }
    }
  }
}
}

int main() {
  try {
    for (const auto form : forms) {
      for (const auto store : {Store::none, Store::accumulator, Store::word}) {
        if (form.word && store == Store::word) { continue; }
        for (const bool knowledge : {false, true}) { exercise(form, store, knowledge); }
      }
    }
    std::cout << checked << " resident RAM operand, physical-alias, CCR and budget cases passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
