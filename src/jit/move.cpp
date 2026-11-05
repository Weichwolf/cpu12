// SPDX-License-Identifier: GPL-3.0-only
#include "jit/move.hpp"

#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/indexed.hpp"

#include <array>
#include <cstdint>

namespace cpu12::detail {
namespace {
Instruction access(const Instruction &move, const MoveOperand operand) {
  return {.kind = Kind::move,
          .length = move.length,
          .addressing = operand.addressing,
          .address = operand.value,
          .word = move.word,
          .indexed = operand.indexed};
}

void address(Code &code, const Instruction &operand, const Memory &ram) {
  if (operand.addressing == Addressing::indexed) {
    indexed_address(code, operand, ram);
  } else {
    constexpr std::array<std::uint8_t, 2> address_r9{0x41, 0xb9};
    code.raw(address_r9);
    code.u32(operand.address);
  }
}

void source_adjustment(Code &code, const Instruction &source, const Instruction &destination) {
  if (source.addressing != Addressing::indexed || destination.addressing != Addressing::indexed ||
      source.indexed.base != destination.indexed.base || source.indexed.update == 0) {
    return;
  }
  // Compute the destination from the logically updated source base without
  // publishing speculative CPU state before an external write can refuse.
  constexpr std::array<std::uint8_t, 3> add_r9{0x41, 0x81, 0xc1};
  constexpr std::array<std::uint8_t, 3> wrap_r9{0x41, 0x81, 0xe1};
  constexpr unsigned mask = 0xffff;
  code.raw(add_r9);
  code.u32(static_cast<std::uint32_t>(source.indexed.update));
  code.raw(wrap_r9);
  code.u32(mask);
}
}

void move_instruction(Code &code, const Instruction &instruction, const Memory &ram) {
  const auto source = access(instruction, instruction.move_source);
  const auto destination = access(instruction, instruction.move_destination);
  if (source.addressing == Addressing::immediate) {
    constexpr std::uint8_t immediate_ecx = 0xb9;
    code.byte(immediate_ecx);
    code.u32(source.address);
  } else {
    address(code, source, ram);
    data_access(code, ram, {.word = instruction.word});
  }
  // Legal move destinations have no indirection: address calculation preserves
  // ECX's captured byte/word, so even the external path needs no host frame.
  address(code, destination, ram);
  source_adjustment(code, source, destination);
  constexpr std::array<std::uint8_t, 2> source_value{0x89, 0xc8};
  code.raw(source_value);
  data_access(code, ram, {.word = instruction.word, .write = true});
  if (source.addressing == Addressing::indexed) { indexed_commit(code, source); }
  if (destination.addressing == Addressing::indexed) { indexed_commit(code, destination); }
  code.advance(instruction.length);
  code.return_();
}
}
