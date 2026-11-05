// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"

#include <cstdint>

namespace cpu12::test::host {
inline constexpr Address iterations = 128;

struct Result {
  Address d = 0, x = 0;
  Byte ccr = 0;
};

struct KernelRegisters {
  Address value = 0;
  Address count = iterations;
  Byte carry = 0;
  Byte negative = 0;
  std::uint32_t flags = 0;
};

inline void arithmetic_kernel(KernelRegisters &state) {
  asm volatile("1:\n\t"
               "addw $3, %w[value]\n\t"
               "setc %[carry]\n\t"
               "xorb $0x5a, %h[value]\n\t"
               "xorb $0x5a, %h[value]\n\t"
               "sets %[negative]\n\t"
               "decw %w[count]\n\t"
               "jnz 1b"
               : [value] "+a"(state.value),
                 [count] "+c"(state.count),
                 [carry] "=&q"(state.carry),
                 [negative] "=&q"(state.negative)
               :
               : "cc");
}

inline void memory_kernel(KernelRegisters &state, Byte &payload) {
  asm volatile("1:\n\t"
               "movb (%[address]), %b[value]\n\t"
               "addb $1, %b[value]\n\t"
               "movb %b[value], (%[address])\n\t"
               "lahf\n\t"
               "movzbl %%ah, %[flags]\n\t"
               "decw %w[count]\n\t"
               "jnz 1b"
               : [value] "+a"(state.value),
                 [count] "+c"(state.count),
                 [flags] "=&d"(state.flags),
                 [payload] "+m"(payload)
               : [address] "D"(&payload)
               : "cc");
}

inline void branches_kernel(KernelRegisters &state) {
  asm volatile("1:\n\tdecw %w[count]\n\tjnz 1b" : [count] "+c"(state.count) : : "cc");
}

inline Result arithmetic(Byte & /*memory*/) {
  KernelRegisters state;
  arithmetic_kernel(state);
  constexpr unsigned negative_shift = 3;
  constexpr unsigned zero_shift = 2;
  return {.d = state.value,
          .x = state.count,
          .ccr = static_cast<Byte>(unsigned{state.carry} |
                                   (unsigned{state.negative} << negative_shift) |
                                   (static_cast<unsigned>(state.count == 0) << zero_shift))};
}

inline Result memory(Byte &payload) {
  KernelRegisters state;
  memory_kernel(state, payload);
  constexpr unsigned byte_mask = 0xff;
  constexpr unsigned byte_bits = 8;
  constexpr unsigned sign = 0x80;
  constexpr unsigned half = 0x10;
  constexpr unsigned carry_mask = 1;
  constexpr unsigned zero = 4;
  return {.d = static_cast<Address>((state.value & byte_mask) << byte_bits),
          .x = state.count,
          .ccr = static_cast<Byte>(((state.flags & sign) >> 4) | ((state.flags & half) << 1) |
                                   (state.flags & carry_mask) | (state.count == 0 ? zero : 0U))};
}

inline Result branches(Byte & /*memory*/) {
  KernelRegisters state;
  branches_kernel(state);
  constexpr Byte zero = 4;
  return {.x = state.count, .ccr = static_cast<Byte>(state.count == 0 ? zero : 0U)};
}
} // namespace cpu12::test::host
