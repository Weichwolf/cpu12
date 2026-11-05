// SPDX-License-Identifier: GPL-3.0-only
#include "jit/emitter.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/flags.hpp"
#include "jit/indexed.hpp"
#include "jit/interrupt.hpp"
#include "jit/loop.hpp"
#include "jit/memory_modify.hpp"
#include "jit/move.hpp"
#include "jit/multiply_divide.hpp"
#include "jit/registers.hpp"
#include "jit/stack.hpp"
#include "jit/transfer.hpp"
#include "runtime.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpu12::detail {
namespace {
constexpr auto d_offset = static_cast<std::uint8_t>(offsetof(Registers, d));
constexpr auto ccr_offset = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr unsigned byte_mask = 0xff;
constexpr unsigned byte_bits = 8;
static_assert(offsetof(Registers, ccr) < 128);
// Instruction templates, in Intel byte order. Only marked immediates/offsets
// are supplied separately; all generated register accesses use the one state.
constexpr std::array<std::uint8_t, 4> entry{0xf3, 0x0f, 0x1e, 0xfa};
constexpr std::array<std::uint8_t, 5> snapshot_flags{0x41, 0x0f, 0x90, 0xc0, 0x9f};
constexpr std::array<std::uint8_t, 4> old_ccr{0x0f, 0xb6, 0x57, ccr_offset};
constexpr std::array<std::uint8_t, 3> copy_ah{0x0f, 0xb6, 0xf4};
constexpr std::array<std::uint8_t, 8> select_nz{0x89, 0xf0, 0xc1, 0xe8, 0x04, 0x83, 0xe0, 0x0c};
constexpr std::array<std::uint8_t, 7> select_c{0x89, 0xf1, 0x83, 0xe1, 0x01, 0x09, 0xc8};
constexpr std::array<std::uint8_t, 8> select_v{0x41, 0x0f, 0xb6, 0xc8, 0xd1, 0xe1, 0x09, 0xc8};
constexpr std::array<std::uint8_t, 7> select_h{0x83, 0xe6, 0x10, 0xd1, 0xe6, 0x09, 0xf0};
constexpr std::array<std::uint8_t, 2> mask_old_ccr{0x83, 0xe2};
constexpr std::array<std::uint8_t, 5> store_ccr{0x09, 0xd0, 0x88, 0x47, ccr_offset};
constexpr std::array<std::uint8_t, 3> load_accumulator{0x0f, 0xb6, 0x47};
constexpr std::array<std::uint8_t, 2> store_accumulator{0x88, 0x47};
constexpr std::array<std::uint8_t, 2> test_accumulator{0x84, 0xc0};
constexpr std::array<std::uint8_t, 4> load_ccr{0x0f, 0xb6, 0x47, ccr_offset};
constexpr std::array<std::uint8_t, 10> signed_condition{
    0x83, 0xe0, 0x0e, 0x89, 0xc1, 0xc1, 0xe9, 0x02, 0x31, 0xc8};

enum Encoding : std::uint8_t {
  ret = 0xc3,
  load_si = 0xbe,
  register_operands = 0xc8,
  move_al_cl = 0x88,
  test_al_cl = 0x84,
  load_al = 0xb0,
  add_al = 0x04,
  adc_al = 0x14,
  sbb_al = 0x1c,
  sub_al = 0x2c,
  cmp_al = 0x3c,
  and_al = 0x24,
  or_al = 0x0c,
  xor_al = 0x34,
  test_al = 0xa8,
  jz = 0x74,
  jnz = 0x75
};

// All call templates retain state/context registers and SysV stack alignment.
constexpr std::array<std::uint8_t, 2> absolute_r11{0x49, 0xbb};
constexpr std::array<std::uint8_t, 9> call_begin{
    0x57, 0x56, 0x48, 0x83, 0xec, 0x08, 0x48, 0x89, 0xf7};
constexpr std::array<std::uint8_t, 3> call_r11{0x41, 0xff, 0xd3};
constexpr std::array<std::uint8_t, 6> call_end{0x48, 0x83, 0xc4, 0x08, 0x5e, 0x5f};
constexpr std::array<std::uint8_t, 8> read_result{0x3d, 0x00, 0x01, 0x00, 0x00, 0x75, 0x01, 0xc3};
constexpr std::array<std::uint8_t, 8> word_read_result{
    0x3d, 0x00, 0x00, 0x01, 0x00, 0x75, 0x01, 0xc3};
constexpr std::array<std::uint8_t, 3> word_write_argument{0x0f, 0xb7, 0xd0};
constexpr std::array<std::uint8_t, 5> write_result{0x85, 0xc0, 0x75, 0x01, 0xc3};
constexpr std::array<std::uint8_t, 2> operand_from_ax{0x89, 0xc1};
constexpr std::array<std::uint8_t, 4> ram_read{0x41, 0x0f, 0xb6, 0x0b};
constexpr std::array<std::uint8_t, 3> ram_write{0x41, 0x88, 0x03};
constexpr std::array<std::uint8_t, 3> write_argument{0x0f, 0xb6, 0xd0};

std::uint8_t *ram_pointer(const Ram ram, const std::uint16_t address) {
  if (address < ram.first) { return nullptr; }
  const auto offset = static_cast<std::size_t>(address - ram.first);
  return offset < ram.bytes.size() ? ram.bytes.data() + offset : nullptr;
}

void bus_call(Code &code, const Instruction &instruction, const bool write) {
  if (write) {
    if (instruction.word) {
      code.raw(word_write_argument);
    } else {
      code.raw(write_argument);
    }
  }
  code.raw(call_begin);
  code.byte(load_si);
  code.u32(instruction.address);
  code.raw(absolute_r11);
  if (write) {
    if (instruction.word) {
      code.u64(reinterpret_cast<std::uintptr_t>(&write_word));
    } else {
      code.u64(reinterpret_cast<std::uintptr_t>(&write_byte));
    }
  } else {
    if (instruction.word) {
      code.u64(reinterpret_cast<std::uintptr_t>(&read_word));
    } else {
      code.u64(reinterpret_cast<std::uintptr_t>(&read_byte));
    }
  }
  code.raw(call_r11);
  code.raw(call_end);
  if (write) {
    code.raw(write_result);
  } else {
    if (instruction.word) {
      code.raw(word_read_result);
    } else {
      code.raw(read_result);
    }
  }
}

void memory_read(Code &code, const Instruction &instruction, const Ram ram) {
  if (instruction.addressing == Addressing::indexed) {
    indexed_read(code, instruction, ram);
    return;
  }
  if (const auto *pointer = ram_pointer(ram, instruction.address)) {
    code.raw(absolute_r11);
    code.u64(reinterpret_cast<std::uintptr_t>(pointer));
    code.raw(ram_read);
    return;
  }
  bus_call(code, instruction, false);
  code.raw(operand_from_ax);
}

void memory_write(Code &code, const Instruction &instruction, const Ram ram) {
  if (instruction.addressing == Addressing::indexed) {
    indexed_write(code, instruction, ram);
    return;
  }
  if (const auto *pointer = ram_pointer(ram, instruction.address)) {
    code.raw(absolute_r11);
    code.u64(reinterpret_cast<std::uintptr_t>(pointer));
    code.raw(ram_write);
    return;
  }
  bus_call(code, instruction, true);
}

}

void flags(Code &code, const FlagPolicy policy, const bool publish) {
  code.raw(snapshot_flags); // seto r8b; lahf
  code.raw(old_ccr);        // movzx edx, old CCR
  code.raw(copy_ah);        // movzx esi, ah
  code.raw(select_nz);
  if (policy.carry) { code.raw(select_c); }
  if (policy.overflow) { code.raw(select_v); }
  if (policy.half) { code.raw(select_h); }
  constexpr unsigned nzv = 0x0e;
  constexpr unsigned carry_mask = 1;
  constexpr unsigned half_mask = 0x20;
  const auto changed = nzv | (policy.carry ? carry_mask : 0U) | (policy.half ? half_mask : 0U);
  code.raw(mask_old_ccr);
  code.byte(static_cast<std::uint8_t>(byte_mask ^ changed));
  if (publish) {
    code.raw(store_ccr);
  } else {
    constexpr std::array<std::uint8_t, 2> merge_old{0x09, 0xd0};
    code.raw(merge_old);
  }
}

void restore_carry(Code &code) {
  code.raw(old_ccr);
  constexpr std::array<std::uint8_t, 2> to_carry{0xd1, 0xea};
  code.raw(to_carry);
}

namespace {

void arithmetic(Code &code, const Instruction &instruction, const Ram ram) {
  const bool immediate = instruction.addressing == Addressing::immediate;
  if (!immediate) { memory_read(code, instruction, ram); }
  const auto offset = static_cast<std::uint8_t>(d_offset + (instruction.accumulator_b ? 0 : 1));
  code.raw(load_accumulator);
  code.byte(offset);
  auto opcode = load_al;
  FlagPolicy policy;
  bool store = true;
  switch (instruction.kind) {
    case Kind::load: break;
    case Kind::add:
      opcode = add_al;
      policy = {.carry = true, .overflow = true, .half = true};
      break;
    case Kind::add_carry:
      opcode = adc_al;
      policy = {.carry = true, .overflow = true, .half = true};
      break;
    case Kind::subtract_carry:
      opcode = sbb_al;
      policy = {.carry = true, .overflow = true};
      break;
    case Kind::subtract:
      opcode = sub_al;
      policy = {.carry = true, .overflow = true};
      break;
    case Kind::compare:
      opcode = cmp_al;
      policy = {.carry = true, .overflow = true};
      store = false;
      break;
    case Kind::bit_and: opcode = and_al; break;
    case Kind::bit_or: opcode = or_al; break;
    case Kind::bit_xor: opcode = xor_al; break;
    case Kind::bit_test:
      opcode = test_al;
      store = false;
      break;
    case Kind::call:
    case Kind::push:
    case Kind::pull:
    case Kind::return_subroutine:
    case Kind::return_interrupt:
    case Kind::register_operation:
    case Kind::transfer:
    case Kind::memory_unary:
    case Kind::loop:
    case Kind::memory_bits:
    case Kind::bit_branch:
    case Kind::move:
    case Kind::multiply_divide:
    case Kind::jump:
    case Kind::effective_address:
    case Kind::nop:
    case Kind::store:
    case Kind::branch:
    default: throw std::logic_error("Invalid native arithmetic kind");
  }
  if (instruction.kind == Kind::add_carry || instruction.kind == Kind::subtract_carry) {
    restore_carry(code);
  }
  if (immediate) {
    code.byte(opcode);
    code.byte(static_cast<std::uint8_t>(instruction.operand));
  } else {
    constexpr unsigned register_form_delta = 4;
    auto native = static_cast<unsigned>(opcode);
    if (opcode == load_al) {
      native = move_al_cl;
    } else if (opcode == test_al) {
      native = test_al_cl;
    } else {
      native -= register_form_delta;
    }
    code.byte(static_cast<std::uint8_t>(native));
    code.byte(register_operands);
  }
  if (instruction.kind == Kind::load) { code.raw(test_accumulator); }
  if (store) {
    code.raw(store_accumulator);
    code.byte(offset);
  }
  flags(code, policy);
  code.advance(instruction.length);
  code.byte(ret);
}

void store(Code &code, const Instruction &instruction, const Ram ram) {
  const auto offset = static_cast<std::uint8_t>(d_offset + (instruction.accumulator_b ? 0 : 1));
  if (instruction.addressing != Addressing::indexed) {
    code.raw(load_accumulator);
    code.byte(offset);
  }
  memory_write(code, instruction, ram);
  // Successful bus writes retain CPU state; reload the source after the ABI call.
  code.raw(load_accumulator);
  code.byte(offset);
  code.raw(test_accumulator);
  flags(code, {});
  code.advance(instruction.length);
  code.byte(ret);
}

constexpr std::array<std::uint8_t, 4> native_word_read{0x41, 0x0f, 0xb7, 0x0b};
constexpr std::array<std::uint8_t, 4> native_word_write{0x66, 0x41, 0x89, 0x03};
constexpr std::array<std::uint8_t, 4> swap_cx{0x66, 0xc1, 0xc1, 0x08};
constexpr std::array<std::uint8_t, 4> swap_ax{0x66, 0xc1, 0xc0, 0x08};
constexpr std::array<std::uint8_t, 3> shift_cx_byte{0xc1, 0xe1, 0x08};
constexpr std::array<std::uint8_t, 4> ram_read_ax{0x41, 0x0f, 0xb6, 0x03};
constexpr std::array<std::uint8_t, 2> merge_cx_ax{0x09, 0xc1};
constexpr std::array<std::uint8_t, 3> load_register_word{0x0f, 0xb7, 0x47};
constexpr std::array<std::uint8_t, 3> store_register_word{0x66, 0x89, 0x47};
constexpr std::array<std::uint8_t, 3> test_register_word{0x66, 0x85, 0xc0};
constexpr std::array<std::uint8_t, 4> word_offsets{
    static_cast<std::uint8_t>(offsetof(Registers, d)),
    static_cast<std::uint8_t>(offsetof(Registers, y)),
    static_cast<std::uint8_t>(offsetof(Registers, x)),
    static_cast<std::uint8_t>(offsetof(Registers, sp))};

enum WordEncoding : std::uint8_t {
  word_prefix = 0x66,
  mov_ax = 0xb8,
  add_ax = 0x05,
  sub_ax = 0x2d,
  cmp_ax = 0x3d,
  mov_ax_cx = 0x89,
  add_ax_cx = 0x01,
  sub_ax_cx = 0x29,
  cmp_ax_cx = 0x39
};

void pointer_constant(Code &code, const std::uint8_t *pointer) {
  code.raw(absolute_r11);
  code.u64(reinterpret_cast<std::uintptr_t>(pointer));
}

void word_memory_read(Code &code, const Instruction &instruction, const Ram ram) {
  if (instruction.addressing == Addressing::indexed) {
    indexed_read(code, instruction, ram);
    return;
  }
  const auto next = static_cast<std::uint16_t>(static_cast<unsigned>(instruction.address) + 1U);
  const auto *high = ram_pointer(ram, instruction.address);
  const auto *low = ram_pointer(ram, next);
  if (high == nullptr || low == nullptr) {
    bus_call(code, instruction, false);
    code.raw(operand_from_ax);
    return;
  }
  pointer_constant(code, high);
  if (low == high + 1) {
    code.raw(native_word_read);
    code.raw(swap_cx);
  } else {
    code.raw(ram_read);
    code.raw(shift_cx_byte);
    pointer_constant(code, low);
    code.raw(ram_read_ax);
    code.raw(merge_cx_ax);
  }
}

void word_memory_write(Code &code, const Instruction &instruction, const Ram ram) {
  if (instruction.addressing == Addressing::indexed) {
    indexed_write(code, instruction, ram);
    return;
  }
  const auto next = static_cast<std::uint16_t>(static_cast<unsigned>(instruction.address) + 1U);
  const auto *high = ram_pointer(ram, instruction.address);
  const auto *low = ram_pointer(ram, next);
  if (high == nullptr || low == nullptr) {
    bus_call(code, instruction, true);
    return;
  }
  code.raw(swap_ax);
  pointer_constant(code, high);
  if (low == high + 1) {
    code.raw(native_word_write);
  } else {
    code.raw(ram_write);
    code.raw(swap_ax);
    pointer_constant(code, low);
    code.raw(ram_write);
  }
}

WordEncoding word_operation(const Instruction &instruction) {
  switch (instruction.kind) {
    case Kind::load: return mov_ax;
    case Kind::add: return add_ax;
    case Kind::subtract: return sub_ax;
    case Kind::compare: return cmp_ax;
    case Kind::add_carry:
    case Kind::subtract_carry:
    case Kind::call:
    case Kind::push:
    case Kind::pull:
    case Kind::return_subroutine:
    case Kind::return_interrupt:
    case Kind::register_operation:
    case Kind::transfer:
    case Kind::memory_unary:
    case Kind::loop:
    case Kind::memory_bits:
    case Kind::bit_branch:
    case Kind::move:
    case Kind::multiply_divide:
    case Kind::jump:
    case Kind::effective_address:
    case Kind::nop:
    case Kind::bit_and:
    case Kind::bit_or:
    case Kind::bit_xor:
    case Kind::bit_test:
    case Kind::branch:
    case Kind::store:
    default: throw std::logic_error("Invalid native word arithmetic kind");
  }
}

WordEncoding word_register_operation(const WordEncoding opcode) {
  switch (opcode) {
    case mov_ax: return mov_ax_cx;
    case add_ax: return add_ax_cx;
    case sub_ax: return sub_ax_cx;
    case cmp_ax: return cmp_ax_cx;
    case word_prefix:
    case mov_ax_cx:
    case add_ax_cx:
    case sub_ax_cx:
    case cmp_ax_cx:
    default: throw std::logic_error("Invalid word instruction encoding");
  }
}

void word_instruction(Code &code, const Instruction &instruction, const Ram ram) {
  const auto offset = word_offsets[static_cast<unsigned>(instruction.target)];
  if (instruction.kind == Kind::store) {
    if (instruction.addressing != Addressing::indexed) {
      code.raw(load_register_word);
      code.byte(offset);
    }
    word_memory_write(code, instruction, ram);
    code.raw(load_register_word);
    code.byte(offset);
    code.raw(test_register_word);
    flags(code, {});
  } else {
    const bool immediate = instruction.addressing == Addressing::immediate;
    if (!immediate) { word_memory_read(code, instruction, ram); }
    code.raw(load_register_word);
    code.byte(offset);
    const auto opcode = word_operation(instruction);
    code.byte(word_prefix);
    if (immediate) {
      code.byte(opcode);
      code.byte(static_cast<std::uint8_t>(instruction.operand & byte_mask));
      code.byte(static_cast<std::uint8_t>(instruction.operand >> byte_bits));
    } else {
      code.byte(word_register_operation(opcode));
      code.byte(register_operands);
    }
    if (instruction.kind == Kind::load) { code.raw(test_register_word); }
    if (instruction.kind != Kind::compare) {
      code.raw(store_register_word);
      code.byte(offset);
    }
    const bool arithmetic = instruction.kind != Kind::load;
    flags(code, {.carry = arithmetic, .overflow = arithmetic});
  }
  code.advance(instruction.length);
  code.byte(ret);
}

void branch(Code &code, const Instruction &instruction) {
  const auto condition = instruction.condition;
  const auto delta =
      instruction.length + static_cast<int>(std::bit_cast<std::int16_t>(instruction.operand));
  if (condition < 2) {
    code.advance(condition == 0 ? delta : instruction.length);
    code.byte(ret);
    return;
  }
  code.raw(load_ccr);
  constexpr unsigned signed_branch = 12;
  if (condition >= signed_branch) { code.raw(signed_condition); }
  constexpr std::array<std::uint8_t, 16> masks{0, 0, 5, 5, 1, 1, 4, 4, 2, 2, 8, 8, 2, 2, 6, 6};
  const bool take_nonzero = (condition & 1U) != 0;
  code.byte(test_al);
  code.byte(masks[condition]);
  code.byte(take_nonzero ? jz : jnz);
  code.byte(7); // Skip exactly the six-byte taken PC update and RET.
  code.advance(delta);
  code.byte(ret);
  code.advance(instruction.length);
  code.byte(ret);
}
}

std::vector<std::uint8_t> emit(const Instruction &instruction, const Ram ram) {
  Code code;
  code.raw(entry);
  if (instruction.kind == Kind::return_interrupt) {
    return_interrupt(code, ram);
  } else if (instruction.kind == Kind::multiply_divide) {
    multiply_divide_instruction(code, instruction);
  } else if (instruction.kind == Kind::move) {
    move_instruction(code, instruction, ram);
  } else if (instruction.kind == Kind::loop) {
    loop_instruction(code, instruction);
  } else if (instruction.kind == Kind::memory_unary || instruction.kind == Kind::memory_bits ||
             instruction.kind == Kind::bit_branch) {
    memory_modify_instruction(code, instruction, ram);
  } else if (instruction.kind == Kind::transfer) {
    transfer_instruction(code, instruction);
  } else if (instruction.kind == Kind::register_operation) {
    register_instruction(code, instruction);
  } else if (instruction.kind == Kind::call) {
    subroutine_call(code, instruction, ram);
  } else if (instruction.kind == Kind::push || instruction.kind == Kind::pull ||
             instruction.kind == Kind::return_subroutine) {
    stack_instruction(code, instruction, ram);
  } else if (instruction.kind == Kind::jump) {
    jump(code, instruction, ram);
  } else if (instruction.kind == Kind::effective_address) {
    indexed_effective_address(code, instruction);
  } else if (instruction.word) {
    word_instruction(code, instruction, ram);
  } else if (instruction.kind == Kind::branch) {
    branch(code, instruction);
  } else if (instruction.kind == Kind::nop) {
    code.advance(1);
    code.byte(ret);
  } else if (instruction.kind == Kind::store) {
    store(code, instruction, ram);
  } else {
    arithmetic(code, instruction, ram);
  }
  return std::move(code).take();
}
}
