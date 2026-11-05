// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"

#include <span>

namespace cpu12::test::checksum {
struct HostResult {
  Address sum;
  Byte carry;
};

inline HostResult
host(const std::span<const Byte> input, const std::span<Byte, 2> output, const Address sum) {
  HostResult result{.sum = sum, .carry = 0};
  auto count = static_cast<Address>(input.size() / 2U);
  const auto *cursor = input.data();
  // Keep the sum in AX, preserve every big-endian accumulator write, and
  // capture only the final ADD carry. LEA/DEC/JNZ and MOV preserve carry.
  asm volatile("1:\n\t"
               "movzwl (%[cursor]), %%ecx\n\t"
               "rolw $8, %%cx\n\t"
               "addw %%cx, %w[sum]\n\t"
               "movb %%ah, (%[output])\n\t"
               "movb %%al, 1(%[output])\n\t"
               "leaq 2(%[cursor]), %[cursor]\n\t"
               "decw %w[count]\n\t"
               "jnz 1b\n\t"
               "setc %[carry]"
               : [cursor] "+S"(cursor),
                 [sum] "+a"(result.sum),
                 [count] "+d"(count),
                 [carry] "=qm"(result.carry)
               : [output] "D"(output.data())
               : "cc", "memory", "rcx");
  return result;
}
}
