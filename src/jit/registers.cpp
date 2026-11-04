// SPDX-License-Identifier: GPL-3.0-only
#include "jit/registers.hpp"

#include "cpu12/core.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/flags.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace cpu12::detail {
namespace {
constexpr auto d_offset = static_cast<std::uint8_t>(offsetof(Registers, d));
constexpr auto ccr_offset = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr std::array<std::uint8_t, 4> load_ccr{0x0f, 0xb6, 0x57, ccr_offset};
constexpr std::array<std::uint8_t, 3> load_byte{0x0f, 0xb6, 0x47};
constexpr std::array<std::uint8_t, 3> load_word{0x0f, 0xb7, 0x47};
constexpr std::array<std::uint8_t, 2> store_byte{0x88, 0x47};
constexpr std::array<std::uint8_t, 3> store_word{0x66, 0x89, 0x47};
constexpr std::array<std::uint8_t, 2> test_byte{0x84, 0xc0};
constexpr std::array<std::uint8_t, 3> test_word{0x66, 0x85, 0xc0};
constexpr std::array<std::uint8_t, 4> carry_out{0x41, 0x0f, 0x92, 0xc1};
constexpr std::array<std::uint8_t, 3> result_nz{0x0f, 0xb6, 0xcc};
constexpr std::array<std::uint8_t, 3> nz_shift{0xc1, 0xe9, 0x04};
constexpr std::array<std::uint8_t, 3> nz_mask{0x83, 0xe1, 0x0c};
constexpr std::array<std::uint8_t, 4> saved_carry{0x41, 0x0f, 0xb6, 0xd1};
constexpr std::array<std::uint8_t, 2> merge_carry{0x09, 0xd1};
constexpr std::array<std::uint8_t, 2> copy_nz{0x89, 0xc8};
constexpr std::array<std::uint8_t, 3> n_shift{0xc1, 0xe8, 0x03};
constexpr std::array<std::uint8_t, 2> n_xor_c{0x31, 0xd0};
constexpr std::array<std::uint8_t, 2> v_shift{0xd1, 0xe0};
constexpr std::array<std::uint8_t, 2> merge_v{0x09, 0xc1};
constexpr std::array<std::uint8_t, 3> preserve_upper{0x83, 0xe2, 0xf0};
constexpr std::array<std::uint8_t, 3> store_ccr{0x88, 0x4f, ccr_offset};

bool shift(const RegisterOp operation) {
  return operation >= RegisterOp::logical_right && operation <= RegisterOp::arithmetic_left;
}

std::array<std::uint8_t, 2> encoding(const RegisterOp operation) {
  constexpr std::array<std::array<std::uint8_t, 2>, 11> native{{{0xf6, 0xd8},
                                                                {0xf6, 0xd0},
                                                                {0xfe, 0xc0},
                                                                {0xfe, 0xc8},
                                                                {0xd0, 0xe8},
                                                                {0xd0, 0xd0},
                                                                {0xd0, 0xd8},
                                                                {0xd0, 0xf8},
                                                                {0xd0, 0xe0},
                                                                {0x30, 0xc0},
                                                                test_byte}};
  const auto index = static_cast<std::size_t>(operation);
  if (index >= native.size()) { throw std::logic_error("Not a unary register operation"); }
  return native[index];
}

void shift_flags(Code &code, const bool word) {
  // x86 rotations retain N/Z and right shifts define V differently. Derive the
  // CPU12 relation V=N xor C from the actual result and saved outgoing carry.
  code.raw(carry_out);
  if (word) {
    code.raw(test_word);
  } else {
    code.raw(test_byte);
  }
  constexpr std::uint8_t lahf = 0x9f;
  code.byte(lahf);
  code.raw(result_nz);
  code.raw(nz_shift);
  code.raw(nz_mask);
  code.raw(saved_carry);
  code.raw(merge_carry);
  code.raw(copy_nz);
  code.raw(n_shift);
  code.raw(n_xor_c);
  code.raw(v_shift);
  code.raw(merge_v);
  code.raw(load_ccr);
  code.raw(preserve_upper);
  code.raw(merge_carry);
  code.raw(store_ccr);
}

void unary(Code &code, const Instruction &instruction) {
  const auto operation = instruction.operation;
  if (instruction.word) {
    code.raw(load_word);
  } else {
    code.raw(load_byte);
  }
  const auto offset =
      static_cast<std::uint8_t>(d_offset + (instruction.word || instruction.accumulator_b ? 0 : 1));
  code.byte(offset);
  if (operation == RegisterOp::rotate_left || operation == RegisterOp::rotate_right) {
    restore_carry(code);
  }
  auto native = encoding(operation);
  if (instruction.word) {
    constexpr std::uint8_t word_prefix = 0x66;
    constexpr std::uint8_t word_shift = 0xd1;
    code.byte(word_prefix);
    native[0] = word_shift;
  }
  code.raw(native);
  if (operation == RegisterOp::complement) { code.raw(test_byte); }
  if (operation != RegisterOp::test) {
    if (instruction.word) {
      code.raw(store_word);
    } else {
      code.raw(store_byte);
    }
    code.byte(offset);
  }
  if (shift(operation)) {
    shift_flags(code, instruction.word);
  } else {
    const bool inc_dec = operation == RegisterOp::increment || operation == RegisterOp::decrement;
    flags(code, {.carry = !inc_dec, .overflow = inc_dec || operation == RegisterOp::negate});
    if (operation == RegisterOp::complement) {
      constexpr std::array<std::uint8_t, 4> force_carry{0x80, 0x4f, ccr_offset, 0x01};
      code.raw(force_carry);
    }
  }
}

void index(Code &code, const Instruction &instruction) {
  const auto offset = static_cast<std::uint8_t>(
      instruction.target == WordTarget::x ? offsetof(Registers, x) : offsetof(Registers, y));
  constexpr std::array<std::uint8_t, 3> add_index{0x66, 0x83, 0x47};
  constexpr std::array<std::uint8_t, 3> set_zero{0x0f, 0x94, 0xc0};
  constexpr std::array<std::uint8_t, 3> place_zero{0xc0, 0xe0, 0x02};
  constexpr std::array<std::uint8_t, 4> remove_zero{0x80, 0x67, ccr_offset, 0xfb};
  constexpr std::array<std::uint8_t, 3> merge_zero{0x08, 0x47, ccr_offset};
  code.raw(add_index);
  code.byte(offset);
  constexpr std::uint8_t minus_one = 0xff;
  code.byte(instruction.operation == RegisterOp::index_increment ? 1 : minus_one);
  code.raw(set_zero);
  code.raw(place_zero);
  code.raw(remove_zero);
  code.raw(merge_zero);
}

void ccr(Code &code, const Instruction &instruction) {
  constexpr std::array<std::uint8_t, 2> and_ccr{0x80, 0x67};
  constexpr std::array<std::uint8_t, 2> or_ccr{0x80, 0x4f};
  const bool conjunction = instruction.operation == RegisterOp::ccr_and;
  if (conjunction) {
    code.raw(and_ccr);
  } else {
    code.raw(or_ccr);
  }
  code.byte(ccr_offset);
  constexpr unsigned retain_x = 0xbf;
  code.byte(static_cast<std::uint8_t>(conjunction ? instruction.operand
                                                  : instruction.operand & retain_x));
}
}

void register_instruction(Code &code, const Instruction &instruction) {
  const auto operation = instruction.operation;
  if (operation == RegisterOp::index_increment || operation == RegisterOp::index_decrement) {
    index(code, instruction);
  } else if (operation == RegisterOp::ccr_and || operation == RegisterOp::ccr_or) {
    ccr(code, instruction);
  } else {
    unary(code, instruction);
  }
  code.advance(instruction.length);
  code.return_();
}
}
