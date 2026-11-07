// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fft_host_kernel.hpp"

#include <cstdint>

namespace cpu12::test::fft {
inline HostResult host_butterfly(const HostMemory &memory) {
  // RDX starts as the scratch pointer and returns the final borrow bit.
  HostResult result{.d = 0, .borrow = reinterpret_cast<std::uintptr_t>(memory.scratch)};
  // Scalar host ASM reuses loaded inputs and coefficients. Keep every original
  // big-endian product, temporary and output store; truncate at guest word width.
  asm volatile(R"(
    .macro cpu12_fft_load offset pointer wide narrow
      movzwl \offset(\pointer),\wide
      rolw $8,\narrow
      movswl \narrow,\wide
    .endm
    .macro cpu12_fft_store value offset pointer
      movl \value,%%r14d
      rolw $8,%%r14w
      movw %%r14w,\offset(\pointer)
    .endm
    .macro cpu12_fft_product
      movl %%eax,%%r14d
      bswapl %%r14d
      movw %%r14w,0(%%rdx)
      shrl $16,%%r14d
      movw %%r14w,2(%%rdx)
      sarl $8,%%eax
      movswl %%ax,%%eax
    .endm
    cpu12_fft_load 0,%%rdi,%%r8d,%%r8w
    cpu12_fft_load 2,%%rdi,%%r9d,%%r9w
    cpu12_fft_load 0,%%rsi,%%r10d,%%r10w
    cpu12_fft_load 2,%%rsi,%%r11d,%%r11w
    cpu12_fft_load 0,%%rcx,%%r12d,%%r12w
    cpu12_fft_load 2,%%rcx,%%r13d,%%r13w
    movl %%r10d,%%eax
    imull %%r12d,%%eax
    cpu12_fft_product
    movl %%eax,%%ebx
    cpu12_fft_store %%ebx,4,%%rdx
    movl %%r11d,%%eax
    imull %%r13d,%%eax
    cpu12_fft_product
    cpu12_fft_store %%eax,8,%%rdx
    subw %%ax,%%bx
    cpu12_fft_store %%ebx,4,%%rdx
    movl %%r10d,%%eax
    imull %%r13d,%%eax
    cpu12_fft_product
    movl %%eax,%%r15d
    cpu12_fft_store %%r15d,6,%%rdx
    movl %%r11d,%%eax
    imull %%r12d,%%eax
    cpu12_fft_product
    addw %%r15w,%%ax
    movl %%eax,%%r15d
    cpu12_fft_store %%r15d,6,%%rdx
    movl %%r8d,%%eax
    cpu12_fft_store %%r8d,10,%%rdx
    addw %%bx,%%r8w
    cpu12_fft_store %%r8d,0,%%rdi
    subw %%bx,%%ax
    cpu12_fft_store %%eax,0,%%rsi
    movl %%r9d,%%eax
    cpu12_fft_store %%r9d,10,%%rdx
    addw %%r15w,%%r9w
    cpu12_fft_store %%r9d,2,%%rdi
    subw %%r15w,%%ax
    setc %%r15b
    cpu12_fft_store %%eax,2,%%rsi
    movzbl %%r15b,%%edx
    .purgem cpu12_fft_load
    .purgem cpu12_fft_store
    .purgem cpu12_fft_product
  )"
               : "=&a"(result.d), "+d"(result.borrow)
               : "D"(memory.u), "S"(memory.v), "c"(memory.coefficient)
               : "cc", "memory", "rbx", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15");
  return result;
}

}
