// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "jit/block.hpp"
#include "runtime.hpp"

#include <cstddef>
#include <cstdint>

#if defined(__APPLE__)
#define CPU12_A64_NAME(name) "_" name
#else
#define CPU12_A64_NAME(name) name
#endif

namespace cpu12::detail {
constexpr unsigned fault_offset = 88;
constexpr unsigned boundary_shift = 24;
static_assert(offsetof(NativeContext, fault) == fault_offset);
static_assert(offsetof(Registers, pc) == 0 && native_page_shift == 8);
static_assert(native_boundary_mask == (1U << boundary_shift));
extern "C" void cpu12_enter_generated(const void *, Registers *, NativeContext *) noexcept;
extern "C" RawNativeResult
cpu12_enter_block(const void *, Registers *, NativeContext *, std::uint32_t) noexcept;
extern "C" RawNativeResult cpu12_run_blocks(
    const void *, Registers *, NativeContext *, std::uint32_t, const void *const *const *) noexcept;

asm(".text\n"
    ".p2align 2\n");
asm(".globl " CPU12_A64_NAME("cpu12_enter_generated") "\n");
asm(CPU12_A64_NAME("cpu12_enter_generated") ":\n");
asm("mov x16,x0\n"
    "mov x0,x1\n"
    "mov x1,x2\n"
    "mov w2,#1\n"
    "br x16\n");
asm(".globl " CPU12_A64_NAME("cpu12_enter_block") "\n");
asm(CPU12_A64_NAME("cpu12_enter_block") ":\n");
asm("mov x16,x0\n"
    "mov x0,x1\n"
    "mov x1,x2\n"
    "mov w2,w3\n"
    "br x16\n");
asm(".globl " CPU12_A64_NAME("cpu12_run_blocks") "\n");
asm(CPU12_A64_NAME("cpu12_run_blocks") ":\n");
asm("stp x29,x30,[sp,#-16]!\n"
    "mov x29,sp\n"
    "stp x19,x20,[sp,#-16]!\n"
    "stp x21,x22,[sp,#-16]!\n"
    "stp x23,x24,[sp,#-16]!\n"
    "mov x16,x0\n"
    "mov x19,x1\n"
    "mov x20,x2\n"
    "mov w21,w3\n"
    "mov x22,x4\n"
    "mov w23,wzr\n"
    "mov x24,xzr\n"
    "Lcpu12_a64_next:\n"
    "mov x0,x19\n"
    "mov x1,x20\n"
    "mov w2,w21\n"
    "blr x16\n"
    "cbz w0,Lcpu12_a64_empty\n"
    "add w23,w23,w0\n"
    "sub w21,w21,w0\n"
    "mov x24,x1\n"
    "tbnz x1,#24,Lcpu12_a64_done\n"
    "cbz w21,Lcpu12_a64_done\n"
    "ldrh w9,[x19]\n"
    "lsr w10,w9,#8\n"
    "ldr x10,[x22,x10,lsl #3]\n"
    "cbz x10,Lcpu12_a64_done\n"
    "and w9,w9,#255\n"
    "ldr x16,[x10,x9,lsl #3]\n"
    "cbnz x16,Lcpu12_a64_next\n"
    "b Lcpu12_a64_done\n"
    "Lcpu12_a64_empty:\n"
    "ldrb w9,[x20,#88]\n"
    "cbz w9,Lcpu12_a64_done\n"
    "mov x24,x1\n"
    "Lcpu12_a64_done:\n"
    "mov w0,w23\n"
    "mov x1,x24\n"
    "ldp x23,x24,[sp],#16\n"
    "ldp x21,x22,[sp],#16\n"
    "ldp x19,x20,[sp],#16\n"
    "ldp x29,x30,[sp],#16\n"
    "ret\n");

NativeResult enter_block(const void *const entry,
                         Registers &registers,
                         NativeContext &context,
                         const std::uint32_t budget) noexcept {
  return native_result(cpu12_enter_block(entry, &registers, &context, budget));
}

NativeResult enter_blocks(const void *const entry,
                          Registers &registers,
                          NativeContext &context,
                          const std::uint32_t budget,
                          const NativeIndex &index) noexcept {
  return native_result(cpu12_run_blocks(entry, &registers, &context, budget, index.data()));
}
}

#undef CPU12_A64_NAME
