// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
namespace {
std::uint16_t word_at(const std::span<const std::uint8_t> bytes, const std::size_t position) {
  constexpr unsigned byte_bits = 8;
  return static_cast<std::uint16_t>((static_cast<unsigned>(bytes[position]) << byte_bits) |
                                    bytes[position + 1]);
}

bool indexed(MoveOperand &operand, const std::uint8_t postbyte) {
  Instruction decoded;
  const std::array bytes{std::uint8_t{0}, postbyte};
  if (!decode_indexed(decoded, bytes) || decoded.indexed.indirect) { return false; }
  operand = {.addressing = Addressing::indexed, .indexed = decoded.indexed};
  return true;
}

bool operands(Instruction &instruction,
              const std::span<const std::uint8_t> bytes,
              const unsigned form) {
  auto &source = instruction.move_source;
  auto &destination = instruction.move_destination;
  switch (form) {
    case 0:
      source.value = instruction.word ? word_at(bytes, 3) : bytes[3];
      return indexed(destination, bytes[2]);
    case 1:
      source = {.addressing = Addressing::extended, .value = word_at(bytes, 3)};
      return indexed(destination, bytes[2]);
    case 2: return indexed(source, bytes[2]) && indexed(destination, bytes[3]);
    case 3:
      source.value = instruction.word ? word_at(bytes, 2) : bytes[2];
      destination = {.addressing = Addressing::extended,
                     .value = word_at(bytes, instruction.word ? 4 : 3)};
      return true;
    case 4:
      source = {.addressing = Addressing::extended, .value = word_at(bytes, 2)};
      destination = {.addressing = Addressing::extended, .value = word_at(bytes, 4)};
      return true;
    case 5:
      destination = {.addressing = Addressing::extended, .value = word_at(bytes, 3)};
      return indexed(source, bytes[2]);
    default: return false;
  }
}
}

std::optional<Instruction> decode_move(const std::span<const std::uint8_t> bytes) {
  constexpr unsigned prefix = 0x18;
  constexpr unsigned first_byte = 8;
  constexpr unsigned last_byte = 13;
  if (bytes.size() < 2 || bytes[0] != prefix) { return {}; }
  const auto opcode = bytes[1];
  const bool word = opcode <= 5;
  if (!word && (opcode < first_byte || opcode > last_byte)) { return {}; }
  const unsigned form = opcode & 7U;
  const std::array<unsigned, 6> lengths{word ? 5U : 4U, 5, 4, word ? 6U : 5U, 6, 5};
  if (bytes.size() != lengths[form]) { return {}; }
  Instruction instruction{.kind = Kind::move,
                          .opcode = bytes[0],
                          .length = static_cast<std::uint8_t>(bytes.size()),
                          .word = word};
  if (!operands(instruction, bytes, form)) { return {}; }
  return instruction;
}
}
