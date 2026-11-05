// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "jit/block.hpp"
#include "runtime.hpp"

#include <cstddef>
#include <cstdint>

#define CPU12_RUN_FAULT_OFFSET 88
#define CPU12_RUN_BOUNDARY_MASK 0x01000000
#define CPU12_RUN_STRING_VALUE(value) #value
#define CPU12_RUN_STRING(value) CPU12_RUN_STRING_VALUE(value)

namespace cpu12::detail {
#if defined(__x86_64__) && defined(__linux__)
// This private assembly ABI is checked against the C++ layout at compilation.
static_assert(offsetof(NativeContext, fault) == CPU12_RUN_FAULT_OFFSET);
static_assert(native_boundary_mask == CPU12_RUN_BOUNDARY_MASK);
static_assert(offsetof(Registers, pc) == 0);
static_assert(sizeof(void *) == 8 && native_page_shift == 8);
static_assert(Fault::none == Fault{} && ExecutionState::running == ExecutionState{});

extern "C" RawNativeResult cpu12_run_blocks(
    const void *, Registers *, NativeContext *, std::uint32_t, const void *const *const *) noexcept;
asm(".equ cpu12_run_fault," CPU12_RUN_STRING(CPU12_RUN_FAULT_OFFSET) "\n");
asm(".equ cpu12_run_boundary," CPU12_RUN_STRING(CPU12_RUN_BOUNDARY_MASK) "\n");
asm(".pushsection .text\n"
    ".globl cpu12_run_blocks\n"
    ".hidden cpu12_run_blocks\n"
    ".type cpu12_run_blocks,@function\n"
    "cpu12_run_blocks:\n"
    "endbr64\n"
    "push %rbx\n"
    "push %rbp\n"
    "push %r12\n"
    "push %r13\n"
    "push %r14\n"
    "push %r15\n"
    "sub $8,%rsp\n"
    "mov %rdi,%rax\n"
    "mov %rsi,%r12\n"
    "mov %rdx,%r13\n"
    "mov %r8,%r14\n"
    "mov %ecx,%r15d\n"
    "xor %ebx,%ebx\n"
    "xor %ebp,%ebp\n"
    ".Lcpu12_run_next:\n"
    "mov %r12,%rdi\n"
    "mov %r13,%rsi\n"
    "mov %r15d,%edx\n"
    "call *%rax\n"
    "test %eax,%eax\n"
    "jz .Lcpu12_run_empty\n"
    "mov %rdx,%rbp\n"
    "add %eax,%ebx\n"
    "sub %eax,%r15d\n"
    "test $cpu12_run_boundary,%edx\n"
    "jnz .Lcpu12_run_done\n"
    "test %r15d,%r15d\n"
    "jz .Lcpu12_run_done\n"
    "movzwl (%r12),%eax\n"
    "mov %eax,%edx\n"
    "shr $8,%edx\n"
    "mov (%r14,%rdx,8),%rdx\n"
    "test %rdx,%rdx\n"
    "jz .Lcpu12_run_done\n"
    "movzbl %al,%eax\n"
    "mov (%rdx,%rax,8),%rax\n"
    "test %rax,%rax\n"
    "jnz .Lcpu12_run_next\n"
    "jmp .Lcpu12_run_done\n"
    ".Lcpu12_run_empty:\n"
    "cmpb $0,cpu12_run_fault(%r13)\n"
    "je .Lcpu12_run_done\n"
    "mov %rdx,%rbp\n"
    ".Lcpu12_run_done:\n"
    "mov %rbp,%rdx\n"
    "mov %ebx,%eax\n"
    "add $8,%rsp\n"
    "pop %r15\n"
    "pop %r14\n"
    "pop %r13\n"
    "pop %r12\n"
    "pop %rbp\n"
    "pop %rbx\n"
    "ret\n"
    ".size cpu12_run_blocks,.-cpu12_run_blocks\n"
    ".popsection\n");

#endif

NativeResult enter_blocks(const void *entry,
                          Registers &registers,
                          NativeContext &context,
                          const std::uint32_t budget,
                          const NativeIndex &index) noexcept {
#if defined(__x86_64__) && defined(__linux__)
  return native_result(cpu12_run_blocks(entry, &registers, &context, budget, index.data()));
#else
  static_cast<void>(entry);
  static_cast<void>(registers);
  static_cast<void>(context);
  static_cast<void>(budget);
  static_cast<void>(index);
  return {};
#endif
}
}

#undef CPU12_RUN_STRING
#undef CPU12_RUN_STRING_VALUE
#undef CPU12_RUN_BOUNDARY_MASK
#undef CPU12_RUN_FAULT_OFFSET
