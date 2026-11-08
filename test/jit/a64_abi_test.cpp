// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "jit/a64/code.hpp"
#include "jit/arena.hpp"
#include "jit/block.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

#if defined(__APPLE__)
#define CPU12_ABI_NAME "_cpu12_a64_check_abi"
#else
#define CPU12_ABI_NAME "cpu12_a64_check_abi"
#endif

extern "C" std::uint64_t cpu12_a64_check_abi(const void *,
                                             cpu12::Registers *,
                                             cpu12::detail::NativeContext *,
                                             std::uint32_t) noexcept;

extern "C" cpu12::detail::RawNativeResult cpu12_run_blocks(const void *,
                                                           cpu12::Registers *,
                                                           cpu12::detail::NativeContext *,
                                                           std::uint32_t,
                                                           const void *const *const *) noexcept;

asm(".text\n.p2align 2\n.globl " CPU12_ABI_NAME "\n" CPU12_ABI_NAME ":\n"
    R"(
    stp x29,x30,[sp,#-16]!
    mov x29,sp
    stp x19,x20,[sp,#-16]!
    stp x21,x22,[sp,#-16]!
    stp x23,x24,[sp,#-16]!
    stp x25,x26,[sp,#-16]!
    stp x27,x28,[sp,#-16]!
    .macro cpu12_abi_seed register value
      mov \register,#\value
      movk \register,#0x1234,lsl #48
    .endm
    cpu12_abi_seed x19,0x119
    cpu12_abi_seed x20,0x120
    cpu12_abi_seed x21,0x121
    cpu12_abi_seed x22,0x122
    cpu12_abi_seed x23,0x123
    cpu12_abi_seed x24,0x124
    cpu12_abi_seed x25,0x125
    cpu12_abi_seed x26,0x126
    cpu12_abi_seed x27,0x127
    cpu12_abi_seed x28,0x128
    mov x16,x0
    mov x0,x1
    mov x1,x2
    mov w2,w3
    blr x16
    mov x10,xzr
    .macro cpu12_abi_check register value shift
      mov x9,#\value
      movk x9,#0x1234,lsl #48
      cmp \register,x9
      cset w11,ne
      orr x10,x10,x11,lsl #\shift
    .endm
    cpu12_abi_check x19,0x119,0
    cpu12_abi_check x20,0x120,1
    cpu12_abi_check x21,0x121,2
    cpu12_abi_check x22,0x122,3
    cpu12_abi_check x23,0x123,4
    cpu12_abi_check x24,0x124,5
    cpu12_abi_check x25,0x125,6
    cpu12_abi_check x26,0x126,7
    cpu12_abi_check x27,0x127,8
    cpu12_abi_check x28,0x128,9
    add x9,sp,#80
    cmp x29,x9
    cset w11,ne
    orr x10,x10,x11,lsl #10
    mov x0,x10
    ldp x27,x28,[sp],#16
    ldp x25,x26,[sp],#16
    ldp x23,x24,[sp],#16
    ldp x21,x22,[sp],#16
    ldp x19,x20,[sp],#16
    ldp x29,x30,[sp],#16
    ret
    .purgem cpu12_abi_seed
    .purgem cpu12_abi_check
    )");

#undef CPU12_ABI_NAME

namespace {
using namespace cpu12;
constexpr Address entry = 0x200, stack = 0x9000;
constexpr Address initial_d = 0xa55a, initial_y = 0x9876;
constexpr std::array<Byte, 2> value{0x7f, 0xff}, one{0, 1}, output{0x40, 0};
constexpr auto arithmetic = isa::program(isa::opcode<isa::Opcode::ldd_immediate>(),
                                         value,
                                         isa::opcode<isa::Opcode::addd_immediate>(),
                                         one,
                                         isa::opcode<isa::Opcode::std_extended>(),
                                         output,
                                         isa::stop);
constexpr auto loop = isa::program(
    isa::dex, isa::opcode<isa::Opcode::bne_relative>(), std::array<Byte, 1>{0xfd}, isa::wai);
constexpr std::array<unsigned, 9> budgets{0, 1, 2, 3, 4, 5, 6, 7, 128};

struct Port {
  unsigned writes = 0;
  Address address = 0;
};

template <class Write> bool reject(void *owner, const Write access) noexcept {
  auto &port = *static_cast<Port *>(owner);
  ++port.writes;
  port.address = access.address;
  return false;
}

void probe(const void *code, const bool external_stack, const Byte flags, const unsigned budget) {
  constexpr Byte disabled_stop = 0x80;
  detail::NativeContext context;
  context.cpu.registers = {
      .pc = entry, .d = initial_d, .x = 3, .y = initial_y, .sp = stack, .ccr = flags};
  Port port;
  context.io = {.owner = &port, .write = reject<ByteWrite>, .write_word = reject<WordWrite>};
  const auto corrupted = cpu12_a64_check_abi(code, &context.cpu.registers, &context, budget);
  if (external_stack && (flags & disabled_stop) == 0 && budget != 0 &&
      (context.fault != Fault::memory_write || port.writes != 1 || port.address != stack - 2)) {
    throw std::runtime_error("ABI fixture did not reach its original frame write fault");
  }
  if (corrupted != 0) {
    std::cerr << "corrupted_mask=" << corrupted << " budget=" << budget << '\n';
    throw std::runtime_error("Native entry corrupted an A64 callee-saved register");
  }
}

void check(const std::span<const Byte> program, const bool external_stack = false) {
  std::array<Byte, address_bytes> ram{};
  std::ranges::copy(program, ram.begin() + entry);
  const auto end = static_cast<Address>(entry + program.size());
  const std::array regions{Ram{.bytes = std::span(ram).first(entry)},
                           Ram{.first = entry,
                               .bytes = std::span(ram).subspan(entry, program.size()),
                               .writable = false},
                           Ram{.first = end, .bytes = std::span(ram).subspan(end)}};
  const auto memory =
      Memory::mapped(external_stack ? std::span(regions).subspan(1, 1) : std::span(regions));
  const auto operations = detail::parse_block(memory, entry);
  detail::ExecutableArena arena;
  const auto *const code = arena.append(detail::emit_block(operations, memory));
  detail::NativePage page{};
  detail::NativeIndex index{};
  page[entry & (detail::native_page_entries - 1)] = code;
  index[entry >> detail::native_page_shift] = page.data();
  using detail::a64::Reg;
  detail::a64::Code dispatch;
  dispatch.move(Reg::x3, Reg::x2);
  dispatch.move(Reg::x2, Reg::x1);
  dispatch.move(Reg::x1, Reg::x0);
  dispatch.constant(Reg::x0, reinterpret_cast<std::uintptr_t>(code));
  dispatch.constant(Reg::x4, reinterpret_cast<std::uintptr_t>(index.data()));
  dispatch.constant(Reg::x16, std::bit_cast<std::uintptr_t>(&cpu12_run_blocks));
  dispatch.jump(Reg::x16);
  const auto *chained = arena.append(dispatch.bytes());
  constexpr Byte disabled_stop = 0x80;
  for (const auto flags : {Byte{0}, disabled_stop}) {
    for (const auto budget : budgets) {
      probe(code, external_stack, flags, budget);
      probe(chained, external_stack, flags, budget);
    }
  }
}
}

int main() {
  try {
    check(isa::nop);
    check(arithmetic);
    check(loop);
    check(isa::stop);
    check(isa::stop, true);
    std::cout << "A64 native entries preserve X19-X29 across grants, dispatch, terminal and fault "
                 "exits\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
