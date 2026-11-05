// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
enum class Kind : std::uint8_t {
  nop,
  load,
  add,
  add_carry,
  subtract,
  subtract_carry,
  compare,
  bit_and,
  bit_or,
  bit_xor,
  bit_test,
  branch,
  store,
  effective_address,
  jump,
  call,
  push,
  pull,
  return_subroutine,
  register_operation,
  transfer,
  memory_unary,
  loop,
  memory_bits,
  bit_branch,
  move,
  multiply_divide,
  return_interrupt,
  software_interrupt,
  wait,
  stop
};

enum class RegisterOp : std::uint8_t {
  negate,
  complement,
  increment,
  decrement,
  logical_right,
  rotate_left,
  rotate_right,
  arithmetic_right,
  arithmetic_left,
  clear,
  test,
  index_increment,
  index_decrement,
  ccr_and,
  ccr_or,
  add_b,
  subtract_b,
  compare_b,
  decimal_adjust
};

enum class Addressing : std::uint8_t { immediate, direct, extended, indexed, relative };
enum class IndexBase : std::uint8_t { x, y, sp, pc };
enum class IndexOffset : std::uint8_t { constant, a, b, d };

struct Indexed {
  IndexBase base = IndexBase::x;
  IndexOffset offset = IndexOffset::constant;
  std::uint16_t displacement = 0;
  std::uint8_t postbyte = 0;
  std::int8_t update = 0;
  bool indirect = false;
};

enum class StackTarget : std::uint8_t { x, y, a, b, ccr, d, pc };

enum class WordTarget : std::uint8_t { d, y, x, sp };

enum class MathOp : std::uint8_t {
  multiply_byte,
  multiply_unsigned,
  multiply_signed,
  divide_unsigned,
  divide_signed,
  divide_extended,
  divide_extended_signed,
  divide_fractional
};

struct MoveOperand {
  Addressing addressing = Addressing::immediate;
  std::uint16_t value = 0;
  Indexed indexed{};
};

struct Instruction {
  Kind kind = Kind::nop;
  std::uint8_t opcode = 0, length = 0;
  std::uint16_t operand = 0;
  bool accumulator_b = false;
  Addressing addressing = Addressing::immediate;
  std::uint16_t address = 0;
  std::uint8_t condition = 0;
  bool word = false;
  WordTarget target = WordTarget::d;
  Indexed indexed{};
  StackTarget stack_target = StackTarget::d;
  RegisterOp operation = RegisterOp::test;
  std::uint8_t bit_mask = 0;
  MoveOperand move_source{}, move_destination{};
  MathOp math = MathOp::multiply_byte;
};

[[nodiscard]] std::optional<Instruction> decode_move(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<Instruction> decode_bits(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<Instruction> decode_loop(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<Instruction> decode_transfer(std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<Instruction> decode_register(std::span<const std::uint8_t> bytes);

[[nodiscard]] bool decode_indexed(Instruction &instruction, std::span<const std::uint8_t> bytes);

[[nodiscard]] std::optional<Instruction> decode(std::span<const std::uint8_t> bytes);
}
