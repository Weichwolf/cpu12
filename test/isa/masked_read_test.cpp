// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction_image.hpp"
#include "isa.hpp"
#include "masked_read_model.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

using cpu12::test::InstructionCore;

namespace {
using namespace cpu12;
using namespace cpu12::test::masked;

struct Form {
  std::array<Byte, 5> code;
  unsigned length, mask_position;
  Address address;
  int relative;
  bool indexed;
};

constexpr std::array<Form, 3> forms{
    {{.code = {static_cast<Byte>(isa::Opcode::brset_direct), 0x40, 0, 2},
      .length = 4,
      .mask_position = 2,
      .address = 0x40,
      .relative = 2,
      .indexed = false},
     {.code = {static_cast<Byte>(isa::Opcode::brset_extended), 0xab, 0xcd, 0, 0x7f},
      .length = 5,
      .mask_position = 3,
      .address = 0xabcd,
      .relative = 127,
      .indexed = false},
     {.code = {static_cast<Byte>(isa::Opcode::brset_indexed), 0x30, 0, 0x80},
      .length = 4,
      .mask_position = 2,
      .address = 0xabcd,
      .relative = -128,
      .indexed = true}}};

struct Observation {
  unsigned value, mask;
  bool set;
};

bool taken(const Observation input) {
  for (unsigned bit = 0; bit < 8; ++bit) {
    const auto selected = 1U << bit;
    if ((input.mask & selected) != 0 && ((input.value & selected) != 0) != input.set) {
      return false;
    }
  }
  return true;
}

std::vector<std::vector<Byte>> catalogue(const Form form) {
  constexpr unsigned maximum = 0xff;
  // The former direct -2 branch landed inside its varied mask operand.
  // Numerical mask coverage uses a separate, declared executable successor.
  std::vector<std::vector<Byte>> operations;
  for (unsigned condition = 0; condition < 2; ++condition) {
    for (unsigned mask = 0; mask <= maximum; ++mask) {
      auto code = form.code;
      code[0] = static_cast<Byte>(code[0] + condition);
      code[form.mask_position] = static_cast<Byte>(mask);
      operations.emplace_back(code.begin(), code.begin() + form.length);
    }
  }
  return operations;
}

void operand_landing_refused() {
  constexpr auto code =
      isa::program(isa::opcode<isa::Opcode::brset_direct>(), std::array<Byte, 3>{0x40, 0, 0xfe});
  const std::array operations{std::vector<Byte>(code.begin(), code.end())};
  constexpr std::array positions{seed.pc};
  bool refused = false;
  try {
    const InstructionCore core(operations, positions);
  } catch (const std::invalid_argument &error) {
    const std::string_view diagnostic(error.what());
    require(diagnostic.contains("variant=0") && diagnostic.contains("PC=65533") &&
                diagnostic.contains("successor=65535") && diagnostic.contains("instruction"),
            "An operand landing refusal identifies its code variant, address and reason");
    refused = true;
  }
  require(refused, "The former capture cannot invent executable code inside its operand");
}

void matrix() {
  Bus bus;
  Io io{.owner = &bus, .read = Bus::read, .read_masked = Bus::masked};
  constexpr unsigned maximum = 0xff;
  for (const auto &form : forms) {
    const auto operations = catalogue(form);
    constexpr std::array positions{seed.pc};
    const std::array<Address, 1> landings{static_cast<Address>(
        static_cast<int>(seed.pc) + static_cast<int>(form.length) + form.relative)};
    InstructionCore core(operations, positions, &io, landings);
    for (unsigned condition = 0; condition < 2; ++condition) {
      auto code = form.code;
      code[0] = static_cast<Byte>(code[0] + condition);
      for (unsigned mask = 0; mask <= maximum; ++mask) {
        code[form.mask_position] = static_cast<Byte>(mask);
        for (unsigned value = 0; value <= maximum; ++value) {
          const auto previous = bus.selected;
          bus.value = static_cast<Byte>(value);
          core.set_registers(seed);
          auto wanted = seed;
          wanted.pc = static_cast<Address>(seed.pc + form.length);
          if (taken({.value = value, .mask = mask, .set = condition == 0})) {
            wanted.pc = static_cast<Address>(static_cast<int>(wanted.pc) + form.relative);
          }
          if (form.indexed) { ++wanted.x; }
          core.select(std::span(code).first(form.length), seed.pc);
          const auto result = core.run(1);
          require(result.retired == 1 && result.fault == Fault::none && result.pc == seed.pc &&
                      core.registers() == wanted && bus.selected == previous + 1 && bus.full == 0 &&
                      bus.writes == 0 && bus.value == 0 && bus.observed.address == form.address &&
                      bus.observed.mask == mask,
                  "Masked branches observe only requested bits through one real destructive read");
          ++checked;
        }
      }
    }
  }
  require(bus.selected == checked,
          "Every mask, including00 andff, preserves a real accepted external access");
}
}

int main() {
  try {
    matrix();
    boundary_cases();
    operand_landing_refused();
    std::cout
        << "Verified " << checked
        << " native selected-bit cases with exact masks, no replay and unchanged ordinary reads.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
