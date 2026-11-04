// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
std::optional<Instruction> decode_loop(const std::span<const std::uint8_t> bytes) {
  constexpr unsigned opcode = 4;
  if (bytes.size() != 3 || bytes[0] != opcode) { return {}; }
  constexpr unsigned selector_mask = 7;
  constexpr unsigned operation_shift = 6;
  const auto selector = bytes[1] & selector_mask;
  const auto operation = static_cast<unsigned>(bytes[1]) >> operation_shift;
  // Selectors 2/3 and operation 3 have no documented loop encoding.
  if (selector == 2 || selector == 3 || operation == 3) { return {}; }
  constexpr unsigned sign_bit = 0x10;
  constexpr unsigned extension = 0xff00;
  constexpr unsigned nonzero = 0x20;
  constexpr std::array operations{RegisterOp::decrement, RegisterOp::test, RegisterOp::increment};
  constexpr std::array targets{WordTarget::d, WordTarget::x, WordTarget::y, WordTarget::sp};
  return Instruction{.kind = Kind::loop,
                     .opcode = opcode,
                     .length = 3,
                     .operand = static_cast<std::uint16_t>(
                         bytes[2] | ((bytes[1] & sign_bit) != 0 ? extension : 0U)),
                     .accumulator_b = selector == 1,
                     .addressing = Addressing::relative,
                     .condition = static_cast<std::uint8_t>((bytes[1] & nonzero) != 0),
                     .word = selector >= 4,
                     .target = targets[selector >= 4 ? selector - 4 : 0],
                     .operation = operations[operation]};
}
}
