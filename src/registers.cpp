// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
namespace {
std::optional<Instruction> page_register(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 2) { return {}; }
  constexpr unsigned prefix = 0x18;
  if (bytes[0] != prefix) { return {}; }
  constexpr unsigned stop = 0x3e;
  if (bytes[1] == stop) { return Instruction{.kind = Kind::stop, .opcode = prefix, .length = 2}; }
  constexpr unsigned add = 6;
  constexpr unsigned decimal = 7;
  constexpr unsigned subtract = 0x16;
  constexpr unsigned compare = 0x17;
  RegisterOp operation;
  switch (bytes[1]) {
    case add: operation = RegisterOp::add_b; break;
    case decimal: operation = RegisterOp::decimal_adjust; break;
    case subtract: operation = RegisterOp::subtract_b; break;
    case compare: operation = RegisterOp::compare_b; break;
    default: return {};
  }
  return Instruction{
      .kind = Kind::register_operation, .opcode = prefix, .length = 2, .operation = operation};
}

std::optional<RegisterOp> operation(const unsigned opcode) {
  enum Opcode : std::uint8_t {
    right_d = 0x49,
    left_d = 0x59,
    clear_a = 0x87,
    clear_b = 0xc7,
    test_a = 0x97,
    test_b = 0xd7,
    logical_or_ccr = 0x14
  };

  constexpr unsigned first_a = 0x40;
  constexpr unsigned last_a = 0x48;
  constexpr unsigned first_b = 0x50;
  constexpr unsigned last_b = 0x58;
  constexpr unsigned low_mask = 15;
  constexpr std::array unary{RegisterOp::negate,
                             RegisterOp::complement,
                             RegisterOp::increment,
                             RegisterOp::decrement,
                             RegisterOp::logical_right,
                             RegisterOp::rotate_left,
                             RegisterOp::rotate_right,
                             RegisterOp::arithmetic_right,
                             RegisterOp::arithmetic_left};
  if ((opcode >= first_a && opcode <= last_a) || (opcode >= first_b && opcode <= last_b)) {
    return unary[opcode & low_mask];
  }
  switch (opcode) {
    case right_d: return RegisterOp::logical_right;
    case left_d: return RegisterOp::arithmetic_left;
    case clear_a:
    case clear_b: return RegisterOp::clear;
    case test_a:
    case test_b: return RegisterOp::test;
    case 0x02:
    case 0x08: return RegisterOp::index_increment;
    case 0x03:
    case 0x09: return RegisterOp::index_decrement;
    case 0x10: return RegisterOp::ccr_and;
    case logical_or_ccr: return RegisterOp::ccr_or;
    default: return {};
  }
}
}

std::optional<Instruction> decode_register(const std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) { return {}; }
  if (const auto instruction = page_register(bytes)) { return instruction; }
  constexpr unsigned nop = 0xa7;
  const auto opcode = bytes[0];
  if (opcode == nop && bytes.size() == 1) {
    return Instruction{.kind = Kind::nop, .opcode = opcode, .length = 1};
  }
  constexpr unsigned rti = 0x0b;
  if (opcode == rti && bytes.size() == 1) {
    return Instruction{.kind = Kind::return_interrupt, .opcode = opcode, .length = 1};
  }
  constexpr unsigned wait = 0x3e;
  if (opcode == wait && bytes.size() == 1) {
    return Instruction{.kind = Kind::wait, .opcode = opcode, .length = 1};
  }
  const auto kind = operation(opcode);
  if (!kind) { return {}; }
  const bool mask = *kind == RegisterOp::ccr_and || *kind == RegisterOp::ccr_or;
  const auto length = mask ? 2U : 1U;
  if (bytes.size() != length) { return {}; }
  constexpr unsigned word_right = 0x49;
  constexpr unsigned word_left = 0x59;
  constexpr unsigned low_index_boundary = 4;
  constexpr unsigned accumulator_b = 0x50;
  constexpr unsigned high_accumulator_b = 0xc7;
  return Instruction{.kind = Kind::register_operation,
                     .opcode = opcode,
                     .length = static_cast<std::uint8_t>(length),
                     .operand = static_cast<std::uint16_t>(mask ? bytes[1] : 0U),
                     .accumulator_b = opcode >= high_accumulator_b ||
                                      (opcode >= accumulator_b && opcode < word_left),
                     .word = opcode == word_right || opcode == word_left,
                     .target = opcode < low_index_boundary ? WordTarget::y : WordTarget::x,
                     .operation = *kind};
}
}
