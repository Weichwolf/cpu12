// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "reference.hpp"
#include "resident_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

namespace {
using namespace cpu12;
using namespace cpu12::test::resident;
constexpr unsigned byte_bits = 8, byte_mask = 0xff, word_values = 65536;
constexpr unsigned word_mask = word_values - 1;
constexpr unsigned flag_c = 1, flag_v = 2, flag_z = 4, flag_n = 8, flags_nzvc = 15;
constexpr Address sign_word = 0x8000;

struct ByteForm {
  Byte encoding = 0;
  unsigned oracle = 0;
  bool b = false;
};

constexpr std::array byte_forms{
    ByteForm{.encoding = isa::opcode<isa::Opcode::ldaa_immediate>()[0], .oracle = test::load_a},
    ByteForm{.encoding = isa::opcode<isa::Opcode::adda_immediate>()[0], .oracle = test::add_a},
    ByteForm{.encoding = isa::opcode<isa::Opcode::suba_immediate>()[0], .oracle = test::sub_a},
    ByteForm{.encoding = isa::opcode<isa::Opcode::cmpa_immediate>()[0], .oracle = test::cmp_a},
    ByteForm{.encoding = isa::opcode<isa::Opcode::anda_immediate>()[0], .oracle = test::and_a},
    ByteForm{.encoding = isa::opcode<isa::Opcode::oraa_immediate>()[0], .oracle = test::or_a},
    ByteForm{.encoding = isa::opcode<isa::Opcode::eora_immediate>()[0], .oracle = test::xor_a},
    ByteForm{.encoding = isa::opcode<isa::Opcode::bita_immediate>()[0], .oracle = test::test_a},
    ByteForm{.encoding = isa::opcode<isa::Opcode::ldab_immediate>()[0],
             .oracle = test::load_a,
             .b = true},
    ByteForm{.encoding = isa::opcode<isa::Opcode::addb_immediate>()[0],
             .oracle = test::add_a,
             .b = true},
    ByteForm{.encoding = isa::opcode<isa::Opcode::subb_immediate>()[0],
             .oracle = test::sub_a,
             .b = true},
    ByteForm{.encoding = isa::opcode<isa::Opcode::cmpb_immediate>()[0],
             .oracle = test::cmp_a,
             .b = true},
    ByteForm{.encoding = isa::opcode<isa::Opcode::andb_immediate>()[0],
             .oracle = test::and_a,
             .b = true},
    ByteForm{
        .encoding = isa::opcode<isa::Opcode::orab_immediate>()[0], .oracle = test::or_a, .b = true},
    ByteForm{.encoding = isa::opcode<isa::Opcode::eorb_immediate>()[0],
             .oracle = test::xor_a,
             .b = true},
    ByteForm{.encoding = isa::opcode<isa::Opcode::bitb_immediate>()[0],
             .oracle = test::test_a,
             .b = true}};
constexpr std::array<Byte, 8> byte_inputs{0, 1, 15, 16, 0x7f, 0x80, 0xfe, 0xff};

struct ByteInput {
  ByteForm form;
  Byte right = 0;
  bool prefix = false;
};

Registers byte_answer(const Registers initial, const ByteInput input) {
  auto input_ccr = unsigned{initial.ccr};
  if (input.prefix) {
    input_ccr &= ~flags_nzvc;
    input_ccr |= (initial.d == 0 ? flag_z : 0U) | (initial.d >= sign_word ? flag_n : 0U);
  }
  const auto left = input.form.b ? initial.d & byte_mask : initial.d >> byte_bits;
  const auto answer = test::expected(
      {.opcode = input.form.oracle, .left = left, .right = input.right, .ccr = input_ccr});
  auto expected = initial;
  expected.pc = static_cast<Address>(entry + (input.prefix ? 5U : 2U));
  if (answer.store) {
    if (input.form.b) {
      expected.d = static_cast<Address>((initial.d & ~byte_mask) | answer.value);
    } else {
      expected.d = static_cast<Address>((answer.value << byte_bits) | (initial.d % test::values));
    }
  }
  expected.ccr = static_cast<Byte>(answer.ccr);
  return expected;
}

void byte_cases(const ByteInput input) {
  const std::array code{isa::opcode<isa::Opcode::cpd_immediate>()[0],
                        Byte{0},
                        Byte{0},
                        input.form.encoding,
                        input.right};
  const auto program = std::span(code).last(input.prefix ? 5U : 2U);
  Device device(program);
  for (const auto left : byte_inputs) {
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      auto initial = seed;
      if (input.form.b) {
        initial.d = static_cast<Address>((seed.d & ~byte_mask) | left);
      } else {
        initial.d = static_cast<Address>((unsigned{left} << byte_bits) | (seed.d % test::values));
      }
      initial.ccr = static_cast<Byte>(ccr);
      device.check(initial, input.prefix ? 2U : 1U, byte_answer(initial, input));
    }
  }
}

void bytes() {
  for (const auto form : byte_forms) {
    for (const bool prefix : {false, true}) {
      for (const auto right : byte_inputs) {
        byte_cases({.form = form, .right = right, .prefix = prefix});
      }
    }
  }
}

void xor_pairs(const ByteForm form) {
  for (const bool prefix : {false, true}) {
    for (const auto operand : byte_inputs) {
      const std::array code{isa::opcode<isa::Opcode::cpd_immediate>()[0],
                            Byte{0},
                            Byte{0},
                            form.encoding,
                            operand,
                            form.encoding,
                            operand};
      const auto program = std::span(code).last(prefix ? 7U : 4U);
      Device device(program);
      for (const auto left : byte_inputs) {
        for (const auto ccr : byte_inputs) {
          auto initial = seed;
          if (form.b) {
            initial.d = static_cast<Address>((seed.d & ~byte_mask) | left);
          } else {
            initial.d = static_cast<Address>((unsigned{left} << byte_bits) | (seed.d & byte_mask));
          }
          initial.ccr = ccr;
          const ByteInput input{.form = form, .prefix = prefix};
          auto expected = byte_answer(initial, input); // XOR zero gives the same final value/flags.
          expected.pc = static_cast<Address>(entry + program.size());
          device.check(initial, prefix ? 3U : 2U, expected);
        }
      }
    }
  }
}

struct WordForm {
  Byte encoding = 0;
  bool subtract = false, compare = false, load = false;
};

constexpr std::array word_forms{
    WordForm{.encoding = isa::opcode<isa::Opcode::addd_immediate>()[0]},
    WordForm{.encoding = isa::opcode<isa::Opcode::subd_immediate>()[0], .subtract = true},
    WordForm{.encoding = isa::opcode<isa::Opcode::cpd_immediate>()[0],
             .subtract = true,
             .compare = true},
    WordForm{.encoding = isa::opcode<isa::Opcode::ldd_immediate>()[0], .load = true}};
constexpr std::array<Address, 8> word_inputs{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xfffe, 0xffff};

std::int32_t signed_word(const Address value) {
  return value < sign_word
             ? value
             : static_cast<std::int32_t>(value) - static_cast<std::int32_t>(word_values);
}

Registers word_answer(const Registers initial, const Address right, const WordForm form) {
  const auto wide = form.subtract ? static_cast<std::int32_t>(initial.d) - right
                                  : static_cast<std::int32_t>(initial.d) + right;
  const auto value =
      form.load ? right : static_cast<Address>(static_cast<unsigned>(wide) & word_mask);
  const auto signed_value = form.subtract ? signed_word(initial.d) - signed_word(right)
                                          : signed_word(initial.d) + signed_word(right);
  unsigned flags = (value == 0 ? flag_z : 0U) | (value >= sign_word ? flag_n : 0U);
  if (!form.load) {
    const bool carry = form.subtract ? wide < 0 : wide >= static_cast<std::int32_t>(word_values);
    if (carry) { flags |= flag_c; }
    flags |= signed_value < -static_cast<std::int32_t>(sign_word) || signed_value >= sign_word
                 ? flag_v
                 : 0U;
  }
  auto expected = initial;
  expected.pc = entry + 3;
  if (!form.compare) { expected.d = value; }
  expected.ccr = static_cast<Byte>((initial.ccr & ~(form.load ? test::nzv : flags_nzvc)) | flags);
  return expected;
}

void words() {
  for (const auto form : word_forms) {
    for (const auto right : word_inputs) {
      const std::array program{form.encoding,
                               static_cast<Byte>(right >> byte_bits),
                               static_cast<Byte>(right & byte_mask)};
      Device device(program);
      for (const auto left : word_inputs) {
        for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
          auto initial = seed;
          initial.d = left;
          initial.ccr = static_cast<Byte>(ccr);
          device.check(initial, 1, word_answer(initial, right, form));
        }
      }
    }
  }
}

struct LoopInput {
  Address counter = 0;
  Byte ccr = 0;
  bool increment = false, y = false;
};

void loop_case(Device &device, const LoopInput input) {
  auto initial = seed;
  (input.y ? initial.y : initial.x) = input.counter;
  initial.ccr = input.ccr;
  auto expected = initial;
  expected.pc = entry + 3;
  (input.y ? expected.y : expected.x) = 0;
  expected.ccr = static_cast<Byte>((input.ccr & ~flag_z) | flag_z);
  unsigned trips = input.counter;
  if (input.increment) { trips = word_values - input.counter; }
  if (input.counter == 0) { trips = word_values; }
  device.check(initial, trips * 2, expected);
  const auto partial = static_cast<Address>(
      (unsigned{input.counter} + (input.increment ? 2U : word_values - 2U)) % word_values);
  (input.y ? expected.y : expected.x) = partial;
  expected.pc = entry + 1;
  expected.ccr = static_cast<Byte>((input.ccr & ~flag_z) | (partial == 0 ? flag_z : 0U));
  if (trips > 1) { device.check(initial, 3, expected); }
}

void loops() {
  constexpr std::array modifiers{isa::opcode<isa::Opcode::dex>()[0],
                                 isa::opcode<isa::Opcode::inx>()[0],
                                 isa::opcode<isa::Opcode::dey>()[0],
                                 isa::opcode<isa::Opcode::iny>()[0]};
  constexpr Byte backedge = 0xfd;
  for (std::size_t index = 0; index < modifiers.size(); ++index) {
    const std::array program{
        modifiers[index], isa::opcode<isa::Opcode::bne_relative>()[0], backedge};
    Device device(program);
    for (const auto counter : word_inputs) {
      for (const auto ccr : byte_inputs) {
        loop_case(device,
                  {.counter = counter, .ccr = ccr, .increment = index % 2 != 0, .y = index >= 2});
      }
    }
  }
}

struct BranchInput {
  std::size_t form = 0;
  Byte ccr = 0, value = 0, displacement = 0;
  bool load = false;
};

void branch_case(const BranchInput input) {
  constexpr std::array forms{isa::opcode<isa::Opcode::bra_relative>()[0],
                             isa::opcode<isa::Opcode::brn_relative>()[0],
                             isa::opcode<isa::Opcode::bne_relative>()[0],
                             isa::opcode<isa::Opcode::beq_relative>()[0]};
  const std::array code{isa::opcode<isa::Opcode::ldaa_immediate>()[0],
                        input.value,
                        forms[input.form],
                        input.displacement};
  const auto program = std::span(code).last(input.load ? 4U : 2U);
  Device device(program);
  auto initial = seed;
  initial.ccr = input.ccr;
  auto expected = initial;
  if (input.load) {
    expected.d = static_cast<Address>((unsigned{input.value} << byte_bits) | (seed.d & byte_mask));
    expected.ccr = static_cast<Byte>((input.ccr & ~test::nzv) | (input.value == 0 ? flag_z : 0U) |
                                     (input.value >= test::sign_bit ? flag_n : 0U));
  }
  const bool zero = (expected.ccr & flag_z) != 0;
  const bool taken = input.form == 0 || (input.form == 2 && !zero) || (input.form == 3 && zero);
  const auto offset = input.displacement < test::sign_bit
                          ? static_cast<int>(input.displacement)
                          : static_cast<int>(input.displacement) - static_cast<int>(test::values);
  const auto next = static_cast<int>(entry + program.size());
  expected.pc = static_cast<Address>(next + (taken ? offset : 0));
  device.check(initial, input.load ? 2U : 1U, expected);
}

void branches() {
  constexpr std::array<Byte, 3> displacements{0x80, 0, 0x7f};
  for (std::size_t form = 0; form < 4; ++form) {
    for (const auto ccr : byte_inputs) {
      for (const auto displacement : displacements) {
        branch_case({.form = form, .ccr = ccr, .displacement = displacement});
        for (const auto value : byte_inputs) {
          branch_case({.form = form,
                       .ccr = ccr,
                       .value = value,
                       .displacement = displacement,
                       .load = true});
        }
      }
    }
  }
}

void memory_words() {
  constexpr std::array<Byte, 2> address{0x40, 0};
  constexpr auto program = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                                        address,
                                        isa::opcode<isa::Opcode::std_extended>(),
                                        address);
  Device device(program);
  for (const auto value : word_inputs) {
    for (const auto ccr : byte_inputs) {
      device.ram[data] = static_cast<Byte>(value >> byte_bits);
      device.ram[data + 1] = static_cast<Byte>(value & byte_mask);
      auto initial = seed;
      initial.ccr = ccr;
      auto expected = initial;
      expected.d = value;
      expected.pc = entry + program.size();
      expected.ccr = static_cast<Byte>((ccr & ~test::nzv) | (value == 0 ? flag_z : 0U) |
                                       (value >= sign_word ? flag_n : 0U));
      device.check(initial, 2, expected);
      require(device.ram[data] == value >> byte_bits && device.ram[data + 1] == (value & byte_mask),
              "Resident word memory accesses retain big-endian byte order");
    }
  }
}
}

int main() {
  try {
    bytes();
    xor_pairs({.encoding = isa::opcode<isa::Opcode::eora_immediate>()[0], .oracle = test::xor_a});
    xor_pairs({.encoding = isa::opcode<isa::Opcode::eorb_immediate>()[0],
               .oracle = test::xor_a,
               .b = true});
    words();
    loops();
    branches();
    memory_words();
    std::cout << "Resident byte/word flags, counter wrap, bounded loops and memory passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
