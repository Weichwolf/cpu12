// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "masked_read_model.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <span>

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

constexpr std::array<Form, 3> forms{{{.code = {0x4e, 0x40, 0, 0xfe},
                                      .length = 4,
                                      .mask_position = 2,
                                      .address = 0x40,
                                      .relative = -2,
                                      .indexed = false},
                                     {.code = {0x1e, 0xab, 0xcd, 0, 0x7f},
                                      .length = 5,
                                      .mask_position = 3,
                                      .address = 0xabcd,
                                      .relative = 127,
                                      .indexed = false},
                                     {.code = {0x0e, 0x30, 0, 0x80},
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

void matrix() {
  Bus bus;
  Io io{.owner = &bus, .read = Bus::read, .read_masked = Bus::masked};
  Core core(Memory::external(), &io);
  constexpr unsigned maximum = 0xff;
  for (const auto &form : forms) {
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
          const auto result = core.run_instruction(std::span(code).first(form.length));
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
  require(core.statistics().native_instructions == checked &&
              core.statistics().read_callbacks == checked,
          "Every mask, including00 andff, preserves a real accepted external access");
}
}

int main() {
  try {
    matrix();
    boundary_cases();
    std::cout
        << "Verified " << checked
        << " native selected-bit cases with exact masks, no replay and unchanged ordinary reads.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
