// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "mandelbrot_program.hpp"

#include <cstdint>

namespace cpu12::test::mandelbrot {
struct HostRegisters {
  std::uint32_t d = 0, high = 0, count;
  Byte flags = 0;
};

inline Registers host(Image &ram, const Point point) {
  HostRegisters result{.count = point.limit};
  // Hold the iteration state in registers, but retain both original product
  // stores and every x/y/square store. Arithmetic shifts implement floor Q8.8.
  asm volatile(R"(
    .macro cpu12_mandel_product
      movl %%eax,%%edx
      shrl $16,%%edx
      movl %%eax,%%r10d
      bswapl %%r10d
      movw %%r10w,12(%%rdi)
      shrl $16,%%r10d
      movw %%r10w,14(%%rdi)
      sarl $8,%%eax
      movswl %%ax,%%eax
    .endm
    .macro cpu12_mandel_store offset
      movl %%eax,%%r10d
      rolw $8,%%r10w
      movw %%r10w,\offset(%%rdi)
    .endm
    movzwl (%%rdi),%%r12d
    rolw $8,%%r12w
    movzwl 2(%%rdi),%%r13d
    rolw $8,%%r13w
    movzwl 4(%%rdi),%%ebx
    rolw $8,%%bx
    movswl %%bx,%%ebx
    movzwl 6(%%rdi),%%ecx
    rolw $8,%%cx
    movswl %%cx,%%ecx
  1:
    movl %%ebx,%%eax
    imull %%ebx,%%eax
    cpu12_mandel_product
    movl %%eax,%%r8d
    cpu12_mandel_store 8
    movl %%ecx,%%eax
    imull %%ecx,%%eax
    cpu12_mandel_product
    movl %%eax,%%r9d
    cpu12_mandel_store 10
    addl %%r8d,%%eax
    cmpl $1024,%%eax
    jae 2f
    movl %%ebx,%%eax
    imull %%ecx,%%eax
    cpu12_mandel_product
    addw %%ax,%%ax
    addw %%r13w,%%ax
    movswl %%ax,%%ecx
    cpu12_mandel_store 6
    movl %%r8d,%%eax
    subw %%r9w,%%ax
    addw %%r12w,%%ax
    setc %%r8b
    movswl %%ax,%%ebx
    cpu12_mandel_store 4
    decw %%si
    jnz 1b
    movzbl %%r8b,%%r8d
    orl $4,%%r8d
    testw %%bx,%%bx
    sets %%r10b
    movzbl %%r10b,%%r10d
    shll $3,%%r10d
    orl %%r10d,%%r8d
    jmp 3f
  2:
    sete %%r8b
    movzbl %%r8b,%%r8d
    shll $2,%%r8d
  3:
    movb %%r8b,%[flags]
    .purgem cpu12_mandel_product
    .purgem cpu12_mandel_store
  )"
               : "=&a"(result.d), "=&d"(result.high), "+S"(result.count), [flags] "=m"(result.flags)
               : "D"(ram.data() + data)
               : "cc", "memory", "rbx", "rcx", "r8", "r9", "r10", "r12", "r13");
  result.flags = static_cast<Byte>(result.flags | (point.ccr & preserved_flags));
  return {.pc = return_pc,
          .d = static_cast<Address>(result.d),
          .x = static_cast<Address>(result.count),
          .y = static_cast<Address>(result.high),
          .sp = stack - 9,
          .ccr = result.flags};
}
}
