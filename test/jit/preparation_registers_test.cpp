// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "preparation_registers.hpp"
#include "unary_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>

namespace cpu12::test::prepared_registers {
namespace {
enum class Target : std::uint8_t { a, b, d };

struct Form {
  Encoding code;
  unary::Function function;
  Target target;
};

constexpr std::array forms{
    Form{.code = code<isa::Opcode::nega>(),
         .function = unary::Function::negate,
         .target = Target::a},
    Form{.code = code<isa::Opcode::negb>(),
         .function = unary::Function::negate,
         .target = Target::b},
    Form{.code = code<isa::Opcode::coma>(),
         .function = unary::Function::complement,
         .target = Target::a},
    Form{.code = code<isa::Opcode::comb>(),
         .function = unary::Function::complement,
         .target = Target::b},
    Form{.code = code<isa::Opcode::inca>(),
         .function = unary::Function::increment,
         .target = Target::a},
    Form{.code = code<isa::Opcode::incb>(),
         .function = unary::Function::increment,
         .target = Target::b},
    Form{.code = code<isa::Opcode::deca>(),
         .function = unary::Function::decrement,
         .target = Target::a},
    Form{.code = code<isa::Opcode::decb>(),
         .function = unary::Function::decrement,
         .target = Target::b},
    Form{
        .code = code<isa::Opcode::lsra>(), .function = unary::Function::right, .target = Target::a},
    Form{
        .code = code<isa::Opcode::lsrb>(), .function = unary::Function::right, .target = Target::b},
    Form{.code = code<isa::Opcode::rola>(),
         .function = unary::Function::rotate_left,
         .target = Target::a},
    Form{.code = code<isa::Opcode::rolb>(),
         .function = unary::Function::rotate_left,
         .target = Target::b},
    Form{.code = code<isa::Opcode::rora>(),
         .function = unary::Function::rotate_right,
         .target = Target::a},
    Form{.code = code<isa::Opcode::rorb>(),
         .function = unary::Function::rotate_right,
         .target = Target::b},
    Form{.code = code<isa::Opcode::asra>(),
         .function = unary::Function::signed_right,
         .target = Target::a},
    Form{.code = code<isa::Opcode::asrb>(),
         .function = unary::Function::signed_right,
         .target = Target::b},
    Form{.code = code<isa::Opcode::asla>(), .function = unary::Function::left, .target = Target::a},
    Form{.code = code<isa::Opcode::aslb>(), .function = unary::Function::left, .target = Target::b},
    Form{
        .code = code<isa::Opcode::clra>(), .function = unary::Function::clear, .target = Target::a},
    Form{
        .code = code<isa::Opcode::clrb>(), .function = unary::Function::clear, .target = Target::b},
    Form{.code = code<isa::Opcode::tsta>(), .function = unary::Function::test, .target = Target::a},
    Form{.code = code<isa::Opcode::tstb>(), .function = unary::Function::test, .target = Target::b},
    Form{
        .code = code<isa::Opcode::lsrd>(), .function = unary::Function::right, .target = Target::d},
    Form{
        .code = code<isa::Opcode::asld>(), .function = unary::Function::left, .target = Target::d}};

struct Point {
  unsigned value;
  Byte ccr;
};

void one(Pair &pair, const std::size_t index, const Point point) {
  const auto value = point.value;
  const auto ccr = point.ccr;
  const auto form = forms[index];
  auto seed = base;
  seed.pc = pair.program.entries[index];
  seed.ccr = ccr;
  if (form.target == Target::a) {
    seed.d = static_cast<Address>((value << byte_bits) | (seed.d & byte_mask));
  } else if (form.target == Target::b) {
    seed.d = static_cast<Address>((seed.d & (byte_mask << byte_bits)) | value);
  } else {
    seed.d = static_cast<Address>(value);
  }
  auto wanted = seed;
  const auto prediction = unary::expected(
      {.function = form.function, .data = value, .ccr = ccr, .word = form.target == Target::d});
  if (prediction.store) {
    if (form.target == Target::a) {
      wanted.d = static_cast<Address>((prediction.data << byte_bits) | (seed.d & byte_mask));
    } else if (form.target == Target::b) {
      wanted.d = static_cast<Address>((seed.d & (byte_mask << byte_bits)) | prediction.data);
    } else {
      wanted.d = static_cast<Address>(prediction.data);
    }
  }
  wanted.ccr = static_cast<Byte>(prediction.flags);
  wanted.pc = static_cast<Address>(wanted.pc + form.code.length);
  check(pair, seed, form.code, {.registers = wanted, .failure = {}}, 1);
}

std::size_t value_cases(Pair &pair, const std::size_t index) {
  const auto &form = forms[index];
  constexpr std::array<Byte, 4> word_flags{0, 1, 0xd0, 0xff};
  std::size_t count = 0;
  const auto maximum = form.target == Target::d ? 0xffffU : byte_mask;
  for (unsigned value = 0; value <= maximum; ++value) {
    if (form.target == Target::d) {
      for (const auto ccr : word_flags) {
        one(pair, index, {.value = value, .ccr = ccr});
        ++count;
      }
    } else {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        one(pair, index, {.value = value, .ccr = static_cast<Byte>(ccr)});
        ++count;
      }
    }
  }
  return count;
}

}

std::size_t unary_cases() {
  constexpr auto codes = [] {
    std::array<Encoding, forms.size()> result{};
    for (std::size_t i = 0; i < forms.size(); ++i) { result[i] = forms[i].code; }
    return result;
  }();
  Pair pair(codes);
  constexpr std::array<unsigned, 12> edges{
      0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0x8080, 0xfffe, 0xffff};
  std::size_t count = 0;
  for (std::size_t index = 0; index < forms.size(); ++index) {
    const auto &form = forms[index];
    count += value_cases(pair, index);
    if (form.target == Target::d) {
      for (const auto value : edges) {
        for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
          one(pair, index, {.value = value, .ccr = static_cast<Byte>(ccr)});
          ++count;
        }
      }
    }
    for (const auto value : edges) {
      auto seed = base;
      seed.pc = pair.program.entries[index];
      seed.d = static_cast<Address>(value);
      seed.ccr = byte_mask;
      prefixes(pair, seed);
    }
  }
  pair.unchanged();
  return count;
}
}

int main() try {
  const auto unary = cpu12::test::prepared_registers::unary_cases();
  const auto ccr = cpu12::test::prepared_registers::ccr_cases();
  const auto accumulator = cpu12::test::prepared_registers::accumulator_cases();
  std::cout << "unary=" << unary << " ccr=" << ccr << " accumulator=" << accumulator
            << " independent LLVM register cases\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
