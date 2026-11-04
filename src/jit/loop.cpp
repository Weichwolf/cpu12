// SPDX-License-Identifier: GPL-3.0-only
#include "jit/loop.hpp"

#include "cpu12/core.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace cpu12::detail {
void loop_instruction(Code &code, const Instruction &instruction) {
  constexpr auto d = static_cast<std::uint8_t>(offsetof(Registers, d));
  constexpr std::array<std::uint8_t, 4> offsets{d,
                                                static_cast<std::uint8_t>(offsetof(Registers, y)),
                                                static_cast<std::uint8_t>(offsetof(Registers, x)),
                                                static_cast<std::uint8_t>(offsetof(Registers, sp))};
  constexpr std::uint8_t compare = 0x7f;
  constexpr std::uint8_t add = 0x47;
  if (instruction.word) {
    constexpr std::array<std::uint8_t, 2> word{0x66, 0x83};
    code.raw(word);
    code.byte(instruction.operation == RegisterOp::test ? compare : add);
    code.byte(offsets[static_cast<unsigned>(instruction.target)]);
  } else {
    constexpr std::uint8_t byte_operation = 0x80;
    code.byte(byte_operation);
    code.byte(instruction.operation == RegisterOp::test ? compare : add);
    code.byte(static_cast<std::uint8_t>(d + (instruction.accumulator_b ? 0 : 1)));
  }
  constexpr std::uint8_t minus_one = 0xff;
  code.byte(instruction.operation == RegisterOp::decrement
                ? minus_one
                : static_cast<std::uint8_t>(instruction.operation == RegisterOp::increment));
  // Use native ZF from the actual-width counter result, never guest CCR.Z.
  // A conditional skip precedes the PC update, which may change host flags.
  constexpr std::uint8_t jz = 0x74;
  constexpr std::uint8_t jnz = 0x75;
  code.byte(instruction.condition != 0 ? jz : jnz);
  code.byte(7); // Six-byte PC update plus RET.
  code.advance(instruction.length +
               static_cast<int>(std::bit_cast<std::int16_t>(instruction.operand)));
  code.return_();
  code.advance(instruction.length);
  code.return_();
}
}
