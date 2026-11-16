// SPDX-License-Identifier: GPL-3.0-only
#include "alu_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction_image.hpp"
#include "isa.hpp"
#include "unary_model.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <vector>

using cpu12::test::InstructionCore;

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

void unary_case(InstructionCore &core, const Case point) {
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

void byte_domain(InstructionCore &core, const Operation operation) {
  for (unsigned value = 0; value <= byte_mask; ++value) {
    for (unsigned flags = 0; flags <= byte_mask; ++flags) {
      unary_case(core, {.operation = operation, .value = value, .flags = flags});
    }
  }
}

void byte_operations(InstructionCore &core) {
  constexpr auto first_a = static_cast<Byte>(isa::Opcode::nega);
  constexpr auto first_b = static_cast<Byte>(isa::Opcode::negb);
  for (unsigned index = 0; index < functions.size(); ++index) {
    byte_domain(core, {.opcode = static_cast<Byte>(first_a + index), .function = functions[index]});
    byte_domain(
        core,
        {.opcode = static_cast<Byte>(first_b + index), .function = functions[index], .b = true});
  }
  constexpr std::array<Operation, 4> other{
      {{.opcode = static_cast<Byte>(isa::Opcode::clra), .function = Function::clear},
       {.opcode = static_cast<Byte>(isa::Opcode::clrb), .function = Function::clear, .b = true},
       {.opcode = static_cast<Byte>(isa::Opcode::tsta), .function = Function::test},
       {.opcode = static_cast<Byte>(isa::Opcode::tstb), .function = Function::test, .b = true}}};
  for (const auto operation : other) { byte_domain(core, operation); }
}

void word_operations(InstructionCore &core) {
  constexpr std::array<Operation, 2> operations{
      {{.opcode = static_cast<Byte>(isa::Opcode::lsrd), .function = Function::right, .word = true},
       {.opcode = static_cast<Byte>(isa::Opcode::asld), .function = Function::left, .word = true}}};
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

void index_operations(InstructionCore &core) {
  struct Index {
    Byte opcode;
    std::uint16_t Registers::*field;
    int delta;
  };

  constexpr std::array<Index, 4> operations{
      {{.opcode = static_cast<Byte>(isa::Opcode::inx), .field = &Registers::x, .delta = 1},
       {.opcode = static_cast<Byte>(isa::Opcode::dex), .field = &Registers::x, .delta = -1},
       {.opcode = static_cast<Byte>(isa::Opcode::iny), .field = &Registers::y, .delta = 1},
       {.opcode = static_cast<Byte>(isa::Opcode::dey), .field = &Registers::y, .delta = -1}}};
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

void ccr_operations(InstructionCore &core) {
  constexpr std::array opcodes{static_cast<Byte>(isa::Opcode::andcc_immediate),
                               static_cast<Byte>(isa::Opcode::orcc_immediate)};
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

}

int main() {
  try {
    std::vector<std::vector<Byte>> operations;
    for (unsigned index = 0; index < functions.size(); ++index) {
      operations.push_back({static_cast<Byte>(static_cast<Byte>(isa::Opcode::nega) + index)});
      operations.push_back({static_cast<Byte>(static_cast<Byte>(isa::Opcode::negb) + index)});
    }
    for (const auto opcode : {isa::Opcode::clra,
                              isa::Opcode::clrb,
                              isa::Opcode::tsta,
                              isa::Opcode::tstb,
                              isa::Opcode::lsrd,
                              isa::Opcode::asld,
                              isa::Opcode::inx,
                              isa::Opcode::dex,
                              isa::Opcode::iny,
                              isa::Opcode::dey}) {
      operations.push_back({static_cast<Byte>(opcode)});
    }
    for (const auto opcode : {isa::Opcode::andcc_immediate, isa::Opcode::orcc_immediate}) {
      for (unsigned mask = 0; mask <= byte_mask; ++mask) {
        operations.push_back({static_cast<Byte>(opcode), static_cast<Byte>(mask)});
      }
    }
    constexpr std::array<Address, 1> positions{seed.pc};
    InstructionCore core(operations, positions);
    byte_operations(core);
    word_operations(core);
    index_operations(core);
    ccr_operations(core);
    std::cout << "Verified " << checked
              << " native unary/shift/index/CCR cases, independent flags "
                 "and complete registers.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
