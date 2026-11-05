// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "reference.hpp"
#include "resident_fixture.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <span>

namespace {
using namespace cpu12;
using namespace cpu12::test::resident;
constexpr unsigned byte_bits = 8, byte_mask = 255, word_mask = 65535;
constexpr unsigned sign = 0x8000, nzv = 14, retained = 0xf0;
constexpr Byte guard = 0xa5;
constexpr std::array<Byte, 2> address{0x40, 0};

struct Form {
  Byte opcode;
  unsigned oracle;
  bool b = false;
};

constexpr std::array forms{
    Form{.opcode = isa::opcode<isa::Opcode::adda_extended>()[0], .oracle = test::add_a},
    Form{.opcode = isa::opcode<isa::Opcode::suba_extended>()[0], .oracle = test::sub_a},
    Form{.opcode = isa::opcode<isa::Opcode::cmpa_extended>()[0], .oracle = test::cmp_a},
    Form{.opcode = isa::opcode<isa::Opcode::anda_extended>()[0], .oracle = test::and_a},
    Form{.opcode = isa::opcode<isa::Opcode::oraa_extended>()[0], .oracle = test::or_a},
    Form{.opcode = isa::opcode<isa::Opcode::eora_extended>()[0], .oracle = test::xor_a},
    Form{.opcode = isa::opcode<isa::Opcode::bita_extended>()[0], .oracle = test::test_a},
    Form{.opcode = isa::opcode<isa::Opcode::addb_extended>()[0], .oracle = test::add_a, .b = true},
    Form{.opcode = isa::opcode<isa::Opcode::subb_extended>()[0], .oracle = test::sub_a, .b = true},
    Form{.opcode = isa::opcode<isa::Opcode::cmpb_extended>()[0], .oracle = test::cmp_a, .b = true},
    Form{.opcode = isa::opcode<isa::Opcode::andb_extended>()[0], .oracle = test::and_a, .b = true},
    Form{.opcode = isa::opcode<isa::Opcode::orab_extended>()[0], .oracle = test::or_a, .b = true},
    Form{.opcode = isa::opcode<isa::Opcode::eorb_extended>()[0], .oracle = test::xor_a, .b = true},
    Form{
        .opcode = isa::opcode<isa::Opcode::bitb_extended>()[0], .oracle = test::test_a, .b = true}};
constexpr std::array<unsigned, 8> values{0, 1, 15, 16, 0x7f, 0x80, 0xfe, 0xff};
constexpr std::array<Byte, 3> conditions{0, 0x25, 0xff};

struct Input {
  unsigned left, right;
  Byte ccr;
};

void check_byte(Device &device, const Form form, const Input input) {
  const auto [left, right, ccr] = input;
  auto initial = seed;
  initial.d = static_cast<Address>(form.b ? (unsigned{guard} << byte_bits) | left
                                          : (left << byte_bits) | guard);
  initial.ccr = ccr;
  const auto answer =
      test::expected({.opcode = form.oracle, .left = left, .right = right, .ccr = ccr});
  auto wanted = initial;
  if (answer.store) {
    wanted.d = static_cast<Address>(form.b ? (unsigned{guard} << byte_bits) | answer.value
                                           : (answer.value << byte_bits) | guard);
  }
  wanted.ccr = static_cast<Byte>((answer.ccr & ~nzv) | (wanted.d == 0 ? 4U : 0U) |
                                 (wanted.d >= sign ? 8U : 0U));
  wanted.pc = entry + 6;
  device.ram[data] = static_cast<Byte>(right);
  device.ram[data - 1] = guard;
  device.ram[data + 2] = guard;
  device.check(initial, 2, wanted);
  require(device.ram[data] == wanted.d >> byte_bits &&
              device.ram[data + 1] == (wanted.d & byte_mask) && device.ram[data - 1] == guard &&
              device.ram[data + 2] == guard,
          "Aliased STD must preserve original arithmetic C/H before replacing the operand");
}

void bytes(const Form form) {
  auto program = isa::program(isa::opcode<isa::Opcode::adda_extended>(),
                              address,
                              isa::opcode<isa::Opcode::std_extended>(),
                              address);
  program[0] = form.opcode;
  Device device(program);
  for (const auto left : values) {
    for (const auto right : values) {
      for (const auto ccr : conditions) {
        check_byte(device, form, {.left = left, .right = right, .ccr = ccr});
      }
    }
  }
}

void check_word(Device &device, const unsigned operation, const Input input) {
  const auto [left, right, ccr] = input;
  const bool add = operation == 0;
  const auto wide = add ? left + right : left + (word_mask + 1) - right;
  const auto result = wide & word_mask;
  const auto overflow =
      add ? (~(left ^ right) & (left ^ result) & sign) : ((left ^ right) & (left ^ result) & sign);
  const bool carry = add ? wide > word_mask : left < right;
  auto initial = seed;
  initial.d = static_cast<Address>(left);
  initial.ccr = ccr;
  auto wanted = initial;
  if (operation != 2) { wanted.d = static_cast<Address>(result); }
  wanted.pc = entry + 3;
  wanted.ccr = static_cast<Byte>((ccr & retained) | unsigned{carry} | (overflow != 0 ? 2U : 0U) |
                                 (result == 0 ? 4U : 0U) | (result >= sign ? 8U : 0U));
  device.ram[data] = static_cast<Byte>(right >> byte_bits);
  device.ram[data + 1] = static_cast<Byte>(right & byte_mask);
  device.check(initial, 1, wanted);
  require(device.ram[data] == right >> byte_bits && device.ram[data + 1] == (right & byte_mask),
          "Memory arithmetic and compare must leave their operand untouched");
}

void words() {
  constexpr std::array opcodes{isa::opcode<isa::Opcode::addd_extended>()[0],
                               isa::opcode<isa::Opcode::subd_extended>()[0],
                               isa::opcode<isa::Opcode::cpd_extended>()[0]};
  constexpr std::array<unsigned, 7> inputs{0, 1, 0x7ffe, 0x7fff, 0x8000, 0xfffe, 0xffff};
  for (unsigned operation = 0; operation < opcodes.size(); ++operation) {
    auto program = isa::program(isa::opcode<isa::Opcode::addd_extended>(), address);
    program[0] = opcodes[operation];
    Device device(program);
    for (const auto left : inputs) {
      for (const auto right : inputs) {
        for (const auto ccr : conditions) {
          check_word(device, operation, {.left = left, .right = right, .ccr = ccr});
        }
      }
    }
  }
}
}

int main() {
  try {
    for (const auto form : forms) { bytes(form); }
    words();
    std::cout
        << "Resident RAM arithmetic preserves independent flags, A/B lanes and aliased operands\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
