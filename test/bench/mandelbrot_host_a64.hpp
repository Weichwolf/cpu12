// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "mandelbrot_program.hpp"

#include <cstdint>

namespace cpu12::test::mandelbrot {
struct HostRegisters {
  std::uint32_t d = 0, high = 0, count, flags = 0;
};

inline HostRegisters host_iterations(Image &ram, const std::uint32_t count) {
  HostRegisters result{.count = count};
  asm volatile(
      R"(
    .macro cpu12_mandel_product
      lsr %w[high],w0,#16
      rev w14,w0
      strh w14,[%[ram],#12]
      lsr w14,w14,#16
      strh w14,[%[ram],#14]
      asr w0,w0,#8
      sxth w0,w0
    .endm
    .macro cpu12_mandel_store offset
      rev16 w14,w0
      strh w14,[%[ram],#\offset]
    .endm
    ldrh w12,[%[ram]]
    rev16 w12,w12
    ldrh w13,[%[ram],#2]
    rev16 w13,w13
    ldrh w8,[%[ram],#4]
    rev16 w8,w8
    sxth w8,w8
    ldrh w9,[%[ram],#6]
    rev16 w9,w9
    sxth w9,w9
  1:
    mul w0,w8,w8
    cpu12_mandel_product
    mov w10,w0
    cpu12_mandel_store 8
    mul w0,w9,w9
    cpu12_mandel_product
    mov w11,w0
    cpu12_mandel_store 10
    add w0,w0,w10
    uxth w0,w0
    cmp w0,#1024
    b.hs 2f
    mul w0,w8,w9
    cpu12_mandel_product
    lsl w0,w0,#1
    add w0,w0,w13
    sxth w9,w0
    cpu12_mandel_store 6
    sub w0,w10,w11
    uxth w0,w0
    add w0,w0,w12
    lsr w16,w0,#16
    sxth w8,w0
    cpu12_mandel_store 4
    sub %w[count],%w[count],#1
    uxth %w[count],%w[count]
    cbnz %w[count],1b
    lsr w15,w8,#31
    lsl w15,w15,#3
    orr w15,w15,w16
    orr w15,w15,#4
    b 3f
  2:
    cset w15,eq
    lsl w15,w15,#2
    sub w14,w0,#1024
    ubfx w16,w14,#15,#1
    orr w15,w15,w16,lsl #3
    eor w16,w0,#1024
    eor w17,w0,w14
    and w16,w16,w17
    ubfx w16,w16,#15,#1
    orr w15,w15,w16,lsl #1
  3:
    mov %w[d],w0
    mov %w[flags],w15
    .purgem cpu12_mandel_product
    .purgem cpu12_mandel_store
  )"
      : [d] "=&r"(result.d),
        [high] "=&r"(result.high),
        [count] "+&r"(result.count),
        [flags] "=&r"(result.flags),
        [written] "+m"(ram)
      : [ram] "r"(ram.data() + data)
      : "cc", "memory", "x0", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
  return result;
}

inline Registers host(Image &ram, const Point point) {
  const auto result = host_iterations(ram, point.limit);
  return {.pc = return_pc,
          .d = static_cast<Address>(result.d),
          .x = static_cast<Address>(result.count),
          .y = static_cast<Address>(result.high),
          .sp = stack - 9,
          .ccr = static_cast<Byte>(result.flags | (point.ccr & preserved_flags))};
}
}
