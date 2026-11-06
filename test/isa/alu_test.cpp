// SPDX-License-Identifier: GPL-3.0-only
#include "alu_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "observed_core.hpp"
#include "unary_model.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
using namespace cpu12::test::alu;
using namespace cpu12::test::unary;

struct Operation {
  Byte opcode;
  Function function;
  bool b = false, word = false;
};

constexpr std::array functions{Function::negate,
                               Function::complement,
                               Function::increment,
                               Function::decrement,
                               Function::right,
                               Function::rotate_left,
                               Function::rotate_right,
                               Function::signed_right,
                               Function::left};

struct Case {
  Operation operation;
  unsigned value, flags;
};

void unary_case(ObservedCore &core, const Case point) {
  const auto op = point.operation;
  auto before = seed;
  before.ccr = static_cast<Byte>(point.flags);
  if (op.word) {
    before.d = static_cast<std::uint16_t>(point.value);
  } else {
    before = accumulator(before, op.b, point.value);
  }
  const auto prediction =
      expected({.function = op.function, .data = point.value, .ccr = point.flags, .word = op.word});
  auto wanted = before;
  wanted.pc = 0;
  wanted.ccr = static_cast<Byte>(prediction.flags);
  if (prediction.store) {
    if (op.word) {
      wanted.d = static_cast<std::uint16_t>(prediction.data);
    } else {
      wanted = accumulator(wanted, op.b, prediction.data);
    }
  }
  retired(core, std::array{op.opcode}, before, wanted);
}

void byte_domain(ObservedCore &core, const Operation operation) {
  for (unsigned value = 0; value <= byte_mask; ++value) {
    for (unsigned flags = 0; flags <= byte_mask; ++flags) {
      unary_case(core, {.operation = operation, .value = value, .flags = flags});
    }
  }
}

void byte_operations(ObservedCore &core) {
  constexpr unsigned first_a = 0x40;
  constexpr unsigned first_b = 0x50;
  for (unsigned index = 0; index < functions.size(); ++index) {
    byte_domain(core, {.opcode = static_cast<Byte>(first_a + index), .function = functions[index]});
    byte_domain(
        core,
        {.opcode = static_cast<Byte>(first_b + index), .function = functions[index], .b = true});
  }
  constexpr std::array<Operation, 4> other{
      {{.opcode = 0x87, .function = Function::clear},
       {.opcode = 0xc7, .function = Function::clear, .b = true},
       {.opcode = 0x97, .function = Function::test},
       {.opcode = 0xd7, .function = Function::test, .b = true}}};
  for (const auto operation : other) { byte_domain(core, operation); }
}

void word_operations(ObservedCore &core) {
  constexpr std::array<Operation, 2> operations{
      {{.opcode = 0x49, .function = Function::right, .word = true},
       {.opcode = 0x59, .function = Function::left, .word = true}}};
  constexpr std::array<unsigned, 4> flags{0, 1, 0xfe, 0xff};
  constexpr unsigned word_values = 65536;
  for (const auto operation : operations) {
    for (unsigned value = 0; value < word_values; ++value) {
      for (const auto ccr : flags) {
        unary_case(core, {.operation = operation, .value = value, .flags = ccr});
      }
    }
  }
}

void index_operations(ObservedCore &core) {
  struct Index {
    Byte opcode;
    std::uint16_t Registers::*field;
    int delta;
  };

  constexpr std::array<Index, 4> operations{{{.opcode = 8, .field = &Registers::x, .delta = 1},
                                             {.opcode = 9, .field = &Registers::x, .delta = -1},
                                             {.opcode = 2, .field = &Registers::y, .delta = 1},
                                             {.opcode = 3, .field = &Registers::y, .delta = -1}}};
  constexpr std::array<unsigned, 4> flags{0, 0xff, 0xaa, 0x55};
  constexpr unsigned word_values = 65536;
  for (const auto operation : operations) {
    for (unsigned value = 0; value < word_values; ++value) {
      for (const auto ccr : flags) {
        auto before = seed;
        before.ccr = static_cast<Byte>(ccr);
        before.*operation.field = static_cast<std::uint16_t>(value);
        auto wanted = before;
        wanted.pc = 0;
        wanted.*operation.field =
            static_cast<std::uint16_t>(static_cast<int>(value) + operation.delta);
        wanted.ccr = static_cast<Byte>((ccr & (byte_mask ^ zero)) |
                                       (wanted.*operation.field == 0 ? zero : 0U));
        retired(core, std::array{operation.opcode}, before, wanted);
      }
    }
  }
}

void ccr_operations(ObservedCore &core) {
  constexpr std::array<Byte, 2> opcodes{0x10, 0x14};
  constexpr unsigned x_mask = 0x40;
  for (const auto opcode : opcodes) {
    for (unsigned mask = 0; mask <= byte_mask; ++mask) {
      for (unsigned value = 0; value <= byte_mask; ++value) {
        auto before = seed;
        before.ccr = static_cast<Byte>(value);
        auto wanted = before;
        wanted.pc = 1;
        wanted.ccr = static_cast<Byte>(
            opcode == opcodes[0] ? value & mask : value | (mask & (byte_mask ^ x_mask)));
        retired(core, std::array{opcode, static_cast<Byte>(mask)}, before, wanted);
      }
    }
  }
}

void refusals(ObservedCore &core) {
  constexpr std::array<Byte, 4> unary{0x40, 0, 0, 0};
  constexpr std::array<Byte, 4> mask{0x14, 0, 0, 0};
  for (const auto input : {unary, mask}) {
    const unsigned valid = input[0] == unary[0] ? 1 : 2;
    for (unsigned length = 1; length <= input.size(); ++length) {
      if (length == valid) { continue; }
      const auto before = core.registers();
      require(core.run_instruction(std::span(input).first(length)).fault ==
                      Fault::unsupported_instruction &&
                  core.registers() == before,
              "Incomplete/padded inherent and CCR instructions cannot reuse valid native captures");
    }
  }
}
}

int main() {
  try {
    ObservedCore core;
    byte_operations(core);
    word_operations(core);
    index_operations(core);
    ccr_operations(core);
    refusals(core);
    require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
            "Inherent ALU and CCR operations execute without external bus access or ISA helpers");
    carry_operations();
    std::cout << "Verified " << checked
              << " native carry/unary/shift/index/CCR cases, independent flags, RAM addressing "
                 "and explicit external faults.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
