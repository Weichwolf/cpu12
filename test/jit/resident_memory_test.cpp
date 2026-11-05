// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "resident_fixture.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <span>

namespace {
using namespace cpu12;
using namespace cpu12::test::resident;
constexpr Address alias_address = 0x6000;
constexpr unsigned byte_bits = 8, byte_mask = 0xff, word_mask = 0xffff;
constexpr unsigned flag_c = 1, flag_z = 4, flag_n = 8, flag_h = 0x20, flags_nzvc = 15;
constexpr std::array<Byte, 2> source{0x40, 0}, destination{0x60, 0}, word_one{0, 1};
constexpr std::array<Byte, 1> byte_one{1}, back_byte{0xf5}, back_word{0xf4}, xor_value{0x5a};
constexpr auto counter = isa::opcode<isa::Opcode::dex>();
constexpr auto branch = isa::opcode<isa::Opcode::bne_relative>();
constexpr auto a_loop = isa::program(isa::opcode<isa::Opcode::ldaa_extended>(),
                                     source,
                                     isa::opcode<isa::Opcode::adda_immediate>(),
                                     byte_one,
                                     isa::opcode<isa::Opcode::staa_extended>(),
                                     destination,
                                     counter,
                                     branch,
                                     back_byte);
constexpr auto b_loop = isa::program(isa::opcode<isa::Opcode::ldab_extended>(),
                                     source,
                                     isa::opcode<isa::Opcode::addb_immediate>(),
                                     byte_one,
                                     isa::opcode<isa::Opcode::stab_extended>(),
                                     destination,
                                     counter,
                                     branch,
                                     back_byte);
constexpr auto word_loop = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                                        source,
                                        isa::opcode<isa::Opcode::addd_immediate>(),
                                        word_one,
                                        isa::opcode<isa::Opcode::std_extended>(),
                                        destination,
                                        counter,
                                        branch,
                                        back_word);
constexpr std::array<Byte, 1> back_changed{0xf3}, back_other{0xf2}, back_partial{0xf1};
constexpr auto changed_loop = isa::program(isa::opcode<isa::Opcode::ldaa_extended>(),
                                           source,
                                           isa::opcode<isa::Opcode::adda_immediate>(),
                                           byte_one,
                                           isa::opcode<isa::Opcode::staa_extended>(),
                                           destination,
                                           isa::opcode<isa::Opcode::eora_immediate>(),
                                           xor_value,
                                           counter,
                                           branch,
                                           back_changed);
constexpr auto other_loop = isa::program(isa::opcode<isa::Opcode::ldaa_extended>(),
                                         source,
                                         isa::opcode<isa::Opcode::adda_immediate>(),
                                         byte_one,
                                         isa::opcode<isa::Opcode::staa_extended>(),
                                         destination,
                                         isa::opcode<isa::Opcode::stab_extended>(),
                                         destination,
                                         counter,
                                         branch,
                                         back_other);
constexpr auto partial_loop = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                                           source,
                                           isa::opcode<isa::Opcode::addd_immediate>(),
                                           word_one,
                                           isa::opcode<isa::Opcode::std_extended>(),
                                           destination,
                                           isa::opcode<isa::Opcode::stab_extended>(),
                                           destination,
                                           counter,
                                           branch,
                                           back_partial);

struct Form {
  std::span<const Byte> program;
  bool b = false, word = false, change = false, other_store = false;
};

constexpr std::array forms{Form{.program = a_loop},
                           Form{.program = b_loop, .b = true},
                           Form{.program = word_loop, .word = true},
                           Form{.program = changed_loop, .change = true},
                           Form{.program = other_loop, .other_store = true},
                           Form{.program = partial_loop, .word = true, .other_store = true}};
constexpr std::array<Address, 8> values{0, 1, 15, 0x7f, 0x80, 0x7fff, 0xfffe, 0xffff};
constexpr std::array<Byte, 8> conditions{0, 1, 15, 0x20, 0x40, 0x80, 0xfe, 0xff};
constexpr std::array<Address, 3> counts{1, 2, 128};

struct Input {
  Form form;
  Address value = 0, count = 0;
  Byte ccr = 0;
};

struct Answer {
  Registers registers;
  Address memory = 0;
};

Answer answer(const Input input) {
  auto registers = seed;
  registers.pc = static_cast<Address>(entry + input.form.program.size());
  registers.x = 0;
  unsigned memory = input.value;
  const auto mask = input.form.word ? word_mask : byte_mask;
  unsigned carry = 0;
  unsigned half = input.ccr & flag_h;
  unsigned negative = 0;
  for (unsigned iteration = 0; iteration < input.count; ++iteration) {
    const auto sum = memory + 1U;
    const auto value = sum & mask;
    carry = sum > mask ? flag_c : 0U;
    if (!input.form.word) { half = (memory % 16 == 15) ? flag_h : 0U; }
    if (input.form.word) {
      registers.d = static_cast<Address>(value);
    } else if (input.form.b) {
      registers.d = static_cast<Address>((seed.d & ~byte_mask) | value);
    } else {
      registers.d = static_cast<Address>((value << byte_bits) | (seed.d & byte_mask));
    }
    memory = value;
    unsigned flag_value = value;
    unsigned sign = input.form.word ? 0x8000U : 0x80U;
    if (input.form.change) {
      constexpr unsigned change = 0x5a00;
      registers.d ^= change;
      flag_value = registers.d >> byte_bits;
    }
    if (input.form.other_store) {
      flag_value = registers.d & byte_mask;
      memory = flag_value;
      if (input.form.word) { memory = (memory << byte_bits) | memory; }
      sign = 0x80;
    }
    negative = flag_value >= sign ? flag_n : 0U;
  }
  registers.ccr =
      static_cast<Byte>((input.ccr & ~(flags_nzvc | flag_h)) | carry | half | negative | flag_z);
  return {.registers = registers, .memory = static_cast<Address>(memory)};
}

void remap(Device &device, const Form form) {
  const auto code_end = static_cast<Address>(entry + form.program.size());
  const std::array regions{
      Ram{.first = 0, .bytes = std::span(device.ram).first(entry)},
      Ram{.first = entry,
          .bytes = std::span(device.ram).subspan(entry, form.program.size()),
          .writable = false},
      Ram{.first = code_end,
          .bytes = std::span(device.ram).subspan(code_end, alias_address - code_end)},
      Ram{.first = alias_address, .bytes = std::span(device.ram).subspan(data, 2)},
      Ram{.first = alias_address + 2, .bytes = std::span(device.ram).subspan(alias_address + 2)}};
  device.core.set_memory(Memory::mapped(regions));
}

void check(Device &device, const Input input) {
  const auto expected = answer(input);
  constexpr Byte guard = 0x33;
  device.ram[data - 1] = guard;
  device.ram[data + 2] = guard;
  device.ram[data] = static_cast<Byte>(input.value & byte_mask);
  device.ram[data + 1] = guard;
  if (input.form.word) {
    device.ram[data] = static_cast<Byte>(input.value >> byte_bits);
    device.ram[data + 1] = static_cast<Byte>(input.value & byte_mask);
  }
  auto initial = seed;
  initial.x = input.count;
  initial.ccr = input.ccr;
  const auto instructions = input.form.change || input.form.other_store ? 6U : 5U;
  device.check(initial, input.count * instructions, expected.registers);
  auto memory = unsigned{device.ram[data]};
  if (input.form.word) { memory = (memory << byte_bits) | device.ram[data + 1]; }
  require(memory == expected.memory && device.ram[data - 1] == guard &&
              device.ram[data + 2] == guard,
          "Loop forwarding changes actual aliased RAM or adjacent bytes");
  require(input.form.word || device.ram[data + 1] == guard,
          "Byte loop writes must not touch the adjacent byte");
}

void run_form(const Form form) {
  Device device(form.program);
  remap(device, form);
  for (const auto count : counts) {
    for (const auto value : values) {
      for (const auto ccr : conditions) {
        check(device,
              {.form = form,
               .value = static_cast<Address>(value & (form.word ? word_mask : byte_mask)),
               .count = count,
               .ccr = ccr});
      }
    }
  }
}
}

int main() {
  try {
    for (const auto form : forms) { run_form(form); }
    std::cout << "Native loop RAM forwarding, physical aliases and non-forwardable stores passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
