// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fft_host_kernel.hpp"

namespace cpu12::test::fft {
inline HostResult host_butterfly(const HostMemory &memory) {
  HostResult result{};
  asm volatile(
      R"(
    .macro cpu12_fft_load offset pointer value
      ldrh w\value,[\pointer,#\offset]
      rev16 w\value,w\value
      sxth w\value,w\value
    .endm
    .macro cpu12_fft_store value offset pointer
      rev16 w14,w\value
      strh w14,[\pointer,#\offset]
    .endm
    .macro cpu12_fft_product
      rev w14,w0
      strh w14,[%[scratch]]
      lsr w14,w14,#16
      strh w14,[%[scratch],#2]
      asr w0,w0,#8
      sxth w0,w0
    .endm
    cpu12_fft_load 0,%[u],8
    cpu12_fft_load 2,%[u],9
    cpu12_fft_load 0,%[v],10
    cpu12_fft_load 2,%[v],11
    cpu12_fft_load 0,%[coefficient],12
    cpu12_fft_load 2,%[coefficient],13
    mul w0,w10,w12
    cpu12_fft_product
    mov w2,w0
    cpu12_fft_store 2,4,%[scratch]
    mul w0,w11,w13
    cpu12_fft_product
    cpu12_fft_store 0,8,%[scratch]
    sub w2,w2,w0
    cpu12_fft_store 2,4,%[scratch]
    mul w0,w10,w13
    cpu12_fft_product
    mov w3,w0
    cpu12_fft_store 3,6,%[scratch]
    mul w0,w11,w12
    cpu12_fft_product
    add w3,w3,w0
    cpu12_fft_store 3,6,%[scratch]
    cpu12_fft_store 8,10,%[scratch]
    add w0,w8,w2
    cpu12_fft_store 0,0,%[u]
    sub w0,w8,w2
    cpu12_fft_store 0,0,%[v]
    cpu12_fft_store 9,10,%[scratch]
    add w0,w9,w3
    cpu12_fft_store 0,2,%[u]
    uxth w14,w9
    uxth w15,w3
    cmp w14,w15
    cset %w[borrow],lo
    sub w0,w9,w3
    cpu12_fft_store 0,2,%[v]
    mov %w[d],w0
    .purgem cpu12_fft_load
    .purgem cpu12_fft_store
    .purgem cpu12_fft_product
  )"
      : [d] "=&r"(result.d), [borrow] "=&r"(result.borrow)
      : [u] "r"(memory.u),
        [v] "r"(memory.v),
        [scratch] "r"(memory.scratch),
        [coefficient] "r"(memory.coefficient)
      : "cc", "memory", "x0", "x2", "x3", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15");
  return result;
}
}
