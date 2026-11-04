// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
namespace {
std::optional<MathOp> operation(const unsigned opcode, const bool page) {
  constexpr unsigned ediv = 0x11;
  constexpr unsigned mul = 0x12;
  constexpr unsigned emul = 0x13;
  constexpr unsigned idiv = 0x10;
  constexpr unsigned edivs = 0x14;
  constexpr unsigned idivs = 0x15;
  if (!page) {
    switch (opcode) {
      case ediv: return MathOp::divide_extended;
      case mul: return MathOp::multiply_byte;
      case emul: return MathOp::multiply_unsigned;
      default: return {};
    }
  }
  switch (opcode) {
    case idiv: return MathOp::divide_unsigned;
    case ediv: return MathOp::divide_fractional;
    case emul: return MathOp::multiply_signed;
    case edivs: return MathOp::divide_extended_signed;
    case idivs: return MathOp::divide_signed;
    default: return {};
  }
}
}

std::optional<Instruction> decode_multiply_divide(const std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) { return {}; }
  constexpr unsigned prefix = 0x18;
  const bool page = bytes[0] == prefix;
  if (bytes.size() != (page ? 2U : 1U)) { return {}; }
  const auto math = operation(bytes[page ? 1U : 0U], page);
  if (!math) { return {}; }
  return Instruction{.kind = Kind::multiply_divide,
                     .opcode = bytes[0],
                     .length = static_cast<std::uint8_t>(bytes.size()),
                     .math = *math};
}
}
