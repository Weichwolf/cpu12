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
};

struct NativeRegisters {
  std::uint32_t value = 0, count = 0, carry = 0;
};

#if defined(__aarch64__)
inline void arithmetic_kernel(NativeRegisters &native) {
  asm volatile(
      "1:\n"
      "uxth %w[value],%w[value]\n"
      "add %w[value],%w[value],#3\n"
      "sub %w[count],%w[count],#1\n"
      "uxth %w[count],%w[count]\n"
      "cbnz %w[count],1b\n"
      "ubfx %w[carry],%w[value],#16,#1\n"
      : [value] "+&r"(native.value), [count] "+&r"(native.count), [carry] "=&r"(native.carry));
}

inline void memory_kernel(NativeRegisters &native, Byte &payload) {
  asm volatile("ldrb %w[value],[%[address]]\n"
               "1:\n"
               "add %w[value],%w[value],#1\n"
               "and %w[value],%w[value],#255\n"
               "strb %w[value],[%[address]]\n"
               "sub %w[count],%w[count],#1\n"
               "uxth %w[count],%w[count]\n"
               "cbnz %w[count],1b\n"
               : [value] "=&r"(native.value), [count] "+&r"(native.count), [payload] "+m"(payload)
               : [address] "r"(&payload)
               : "memory");
}

inline void branches_kernel(NativeRegisters &native) {
  asm volatile("1:\n"
               "sub %w[count],%w[count],#1\n"
               "uxth %w[count],%w[count]\n"
               "cbnz %w[count],1b"
               : [count] "+r"(native.count));
}
#endif

inline void arithmetic_kernel(KernelRegisters &state) {
#if defined(__aarch64__)
  NativeRegisters native{.value = state.value, .count = state.count};
  arithmetic_kernel(native);
  state.value = static_cast<Address>(native.value);
  state.count = static_cast<Address>(native.count);
  state.carry = static_cast<Byte>(native.carry);
#else
  // The two guest EORAs cancel the value and leave V=0. DEX supplies final Z.
  // ADDD supplies final N/C; native DEC and JNZ preserve that last carry.
  asm volatile("1:\n\t"
               "addw $3, %w[value]\n\t"
               "decw %w[count]\n\t"
               "jnz 1b\n\t"
               "setc %[carry]"
               : [value] "+a"(state.value), [count] "+c"(state.count), [carry] "=&q"(state.carry)
               :
               : "cc");
#endif
}

inline void memory_kernel(KernelRegisters &state, Byte &payload) {
#if defined(__aarch64__)
  NativeRegisters native{.count = state.count};
  memory_kernel(native, payload);
  state.value = static_cast<Address>(native.value);
  state.count = static_cast<Address>(native.count);
#else
  // The preceding store supplies the next load in this owned ordinary-RAM loop.
  // Retain the byte in AL, preserve every write, and materialize final flags once.
  asm volatile("movzbl (%[address]), %k[value]\n\t"
               "1:\n\t"
               "addb $1, %b[value]\n\t"
               "movb %b[value], (%[address])\n\t"
               "decw %w[count]\n\t"
               "jnz 1b"
               : [value] "=&a"(state.value), [count] "+c"(state.count), [payload] "+m"(payload)
               : [address] "D"(&payload)
               : "cc");
#endif
}

inline void branches_kernel(KernelRegisters &state) {
#if defined(__aarch64__)
  NativeRegisters native{.count = state.count};
  branches_kernel(native);
  state.count = static_cast<Address>(native.count);
#else
  asm volatile("1:\n\tdecw %w[count]\n\tjnz 1b" : [count] "+c"(state.count) : : "cc");
#endif
}

inline Result arithmetic_result(const KernelRegisters state) {
  constexpr unsigned sign = 0x8000;
  constexpr unsigned negative_shift = 12;
  constexpr unsigned zero = 4;
  return {.d = state.value,
          .x = state.count,
          .ccr =
              static_cast<Byte>(unsigned{state.carry} | ((state.value & sign) >> negative_shift) |
                                (state.count == 0 ? zero : 0U))};
}

inline Result memory_result(const KernelRegisters state) {
  constexpr unsigned byte_mask = 0xff;
  constexpr unsigned byte_bits = 8;
  constexpr unsigned sign = 0x80;
  constexpr unsigned half = 0x20;
  constexpr unsigned low_nibble = 15;
  constexpr unsigned carry = 1;
  constexpr unsigned zero = 4;
  const auto value = state.value & byte_mask;
  // Last ADDA #1: C iff the byte wrapped, H iff its low nibble wrapped.
  // STAA clears V and DEX supplies final Z; earlier flag values are dead.
  return {.d = static_cast<Address>(value << byte_bits),
          .x = state.count,
          .ccr = static_cast<Byte>(((value & sign) >> 4) | ((value & low_nibble) == 0 ? half : 0U) |
                                   (value == 0 ? carry : 0U) | (state.count == 0 ? zero : 0U))};
}

inline Result arithmetic(Byte & /*memory*/) {
  KernelRegisters state;
  arithmetic_kernel(state);
  return arithmetic_result(state);
}

inline Result control_result(const KernelRegisters state) {
  auto result = arithmetic_result(state);
  // ADDD #3 overflow is live through DEX and both branches. Rematerialize its
  // final value instead of saving host flags on every loop iteration.
  constexpr Address first_overflow = 0x8000;
  constexpr Address last_overflow = 0x8002;
  constexpr Byte overflow = 2;
  if (state.value >= first_overflow && state.value <= last_overflow) {
    result.ccr = static_cast<Byte>(result.ccr | overflow);
  }
  return result;
}

inline Result control(Byte & /*memory*/) {
  KernelRegisters state;
  arithmetic_kernel(state);
  return control_result(state);
}

inline Result counted(Byte & /*memory*/) {
  KernelRegisters state;
  arithmetic_kernel(state);
  auto result = control_result(state);
  constexpr Byte zero = 4;
  result.ccr = static_cast<Byte>((result.ccr & ~zero) | (state.value == 0 ? zero : 0));
  return result;
}

inline Result memory(Byte &payload) {
  KernelRegisters state;
  memory_kernel(state, payload);
  return memory_result(state);
}

inline Result branches(Byte & /*memory*/) {
  KernelRegisters state;
  branches_kernel(state);
  constexpr Byte zero = 4;
  return {.x = state.count, .ccr = static_cast<Byte>(state.count == 0 ? zero : 0U)};
}
} // namespace cpu12::test::host
