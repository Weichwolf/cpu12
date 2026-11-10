// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"

#include <cstdint>
#include <span>

namespace cpu12::test::checksum {
struct HostResult {
  Address sum;
  Byte carry;
};

struct HostWordRegisters {
  std::uint32_t sum, count, carry = 0;
};

#if defined(__aarch64__)
inline void
host_sum(HostWordRegisters &native, const Byte *cursor, const std::span<Byte, 2> output) {
  asm volatile("1:\n"
               "ldrh w9,[%[cursor]],#2\n"
               "rev16 w9,w9\n"
               "uxth %w[sum],%w[sum]\n"
               "add %w[sum],%w[sum],w9\n"
               "rev16 w9,%w[sum]\n"
               "strh w9,[%[output]]\n"
               "sub %w[count],%w[count],#1\n"
               "uxth %w[count],%w[count]\n"
               "cbnz %w[count],1b\n"
               "ubfx %w[carry],%w[sum],#16,#1"
               : [cursor] "+&r"(cursor),
                 [sum] "+&r"(native.sum),
                 [count] "+&r"(native.count),
                 [carry] "=&r"(native.carry)
               : [output] "r"(output.data())
               : "memory", "x9");
}
#endif

inline HostResult
host_cpp(const std::span<const Byte> input, const std::span<Byte, 2> output, const Address seed) {
  constexpr unsigned byte_bits = 8;
  constexpr unsigned word_bits = 16;
  constexpr unsigned byte_mask = 0xff;
  HostResult result{.sum = seed, .carry = 0};
  for (std::size_t index = 0; index < input.size(); index += 2) {
    const auto word = (unsigned{input[index]} << byte_bits) | input[index + 1];
    const auto sum = unsigned{result.sum} + word;
    result.sum = static_cast<Address>(sum);
    result.carry = static_cast<Byte>(sum >> word_bits);
    output[0] = static_cast<Byte>(result.sum >> byte_bits);
    output[1] = static_cast<Byte>(result.sum & byte_mask);
  }
  return result;
}

inline HostResult
host(const std::span<const Byte> input, const std::span<Byte, 2> output, const Address sum) {
  HostResult result{.sum = sum, .carry = 0};
#if defined(__aarch64__)
  HostWordRegisters native{.sum = sum, .count = static_cast<Address>(input.size() / 2U)};
  host_sum(native, input.data(), output);
  result.sum = static_cast<Address>(native.sum);
  result.carry = static_cast<Byte>(native.carry);
#else
  auto count = static_cast<Address>(input.size() / 2U);
  const auto *cursor = input.data();
  // Keep the sum in AX, preserve every big-endian accumulator write, and
  // capture only the final ADD carry. LEA/DEC/JNZ and MOV preserve carry.
  asm volatile("1:\n\t"
               "movzwl (%[cursor]), %%ecx\n\t"
               "rolw $8, %%cx\n\t"
               "addw %%cx, %w[sum]\n\t"
#if defined(__MOVBE__)
               "movbew %w[sum], (%[output])\n\t"
#else
               "movb %%ah, (%[output])\n\t"
               "movb %%al, 1(%[output])\n\t"
#endif
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
#endif
  return result;
}
}
