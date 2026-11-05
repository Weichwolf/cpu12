// SPDX-License-Identifier: GPL-3.0-only
#include "jit/memory_modify.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/flags.hpp"
#include "jit/indexed.hpp"
#include "jit/registers.hpp"
#include "memory_access.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace cpu12::detail {
namespace {
constexpr auto ccr_offset = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr std::array<std::uint8_t, 2> operand{0x89, 0xc8};
constexpr std::array<std::uint8_t, 5> save_address{0x66, 0x44, 0x89, 0x0c, 0x24};
constexpr std::array<std::uint8_t, 5> restore_address{0x44, 0x0f, 0xb7, 0x0c, 0x24};
constexpr std::array<std::uint8_t, 5> save_context{0x48, 0x89, 0x74, 0x24, 0x08};
constexpr std::array<std::uint8_t, 5> restore_context{0x48, 0x8b, 0x74, 0x24, 0x08};
constexpr std::array<std::uint8_t, 4> save_result{0x88, 0x44, 0x24, 0x02};
constexpr std::array<std::uint8_t, 4> save_flags{0x88, 0x44, 0x24, 0x03};
constexpr std::array<std::uint8_t, 5> restore_result{0x0f, 0xb6, 0x44, 0x24, 0x02};
constexpr std::array<std::uint8_t, 5> restore_flags{0x0f, 0xb6, 0x44, 0x24, 0x03};
constexpr std::array<std::uint8_t, 3> publish_flags{0x88, 0x47, ccr_offset};

void operation(Code &code, const Instruction &instruction) {
  if (instruction.kind == Kind::memory_bits) {
    constexpr std::uint8_t or_byte = 0x0c;
    constexpr std::uint8_t and_byte = 0x24;
    code.byte(instruction.condition == 0 ? or_byte : and_byte);
    code.byte(instruction.condition == 0 ? instruction.bit_mask
                                         : static_cast<std::uint8_t>(~instruction.bit_mask));
  } else {
    unary_operation(code, instruction);
  }
}

void result_flags(Code &code, const Instruction &instruction, const bool publish = true) {
  if (instruction.kind == Kind::memory_bits) {
    flags(code, {}, publish);
  } else {
    unary_flags(code, instruction, publish);
  }
}

void address(Code &code, const Instruction &instruction, const Memory &ram) {
  if (instruction.addressing == Addressing::indexed) {
    indexed_address(code, instruction, ram);
  } else {
    constexpr std::array<std::uint8_t, 2> immediate_r9{0x41, 0xb9};
    code.raw(immediate_r9);
    code.u32(instruction.address);
  }
}

void finish(Code &code, const Instruction &instruction) {
  if (instruction.addressing == Addressing::indexed) { indexed_commit(code, instruction); }
  code.advance(instruction.length);
  code.return_();
}

void single_access(Code &code, const Instruction &instruction, const Memory &ram) {
  if (instruction.operation == RegisterOp::test) {
    data_access(code, ram, {});
    code.raw(operand);
  } else {
    // CLR is write-only. Recreate its flags after accepted access because the
    // bus call or partial-window guard may change the host's arithmetic flags.
    operation(code, instruction);
    data_access(code, ram, {.write = true});
  }
  operation(code, instruction);
  result_flags(code, instruction);
  finish(code, instruction);
}

void bit_branch(Code &code, const Instruction &instruction, const Memory &ram) {
  data_access(code, ram, {.selected = true, .mask = instruction.bit_mask});
  code.raw(operand);
  if (instruction.condition == 0) {
    constexpr std::array<std::uint8_t, 2> complement{0xf6, 0xd0};
    code.raw(complement);
  }
  constexpr std::uint8_t test = 0xa8;
  code.byte(test);
  code.byte(instruction.bit_mask);
  constexpr std::array<std::uint8_t, 2> not_zero{0x0f, 0x85};
  code.raw(not_zero);
  const auto not_taken = code.relative32();
  // Decide before committing the auto-index change, which alters host flags.
  if (instruction.addressing == Addressing::indexed) { indexed_commit(code, instruction); }
  code.advance(instruction.length +
               static_cast<int>(std::bit_cast<std::int16_t>(instruction.operand)));
  code.return_();
  code.bind(not_taken);
  finish(code, instruction);
}

void known_ram(Code &code, const Instruction &instruction, const Ram ram) {
  native_data_access(code, ram, {});
  code.raw(operand);
  operation(code, instruction);
  // Byte stores retain the native result flags; no second bounds check or call.
  native_data_access(code, ram, {.write = true});
  result_flags(code, instruction);
  finish(code, instruction);
}

void external(Code &code, const Instruction &instruction, const Memory &memory) {
  // Only the bus path needs staged address/result/flags. Fault epilogues release
  // this host frame; accepted external effects are never rolled back or replayed.
  code.frame();
  code.raw(save_address);
  data_access(code, memory, {});
  code.raw(operand);
  operation(code, instruction);
  code.raw(save_result);
  code.raw(save_context);
  result_flags(code, instruction, false);
  code.raw(save_flags);
  code.raw(restore_context);
  code.raw(restore_address);
  code.raw(restore_result);
  data_access(code, memory, {.write = true});
  code.raw(restore_flags);
  code.raw(publish_flags);
  finish(code, instruction);
}
}

void memory_modify_instruction(Code &code, const Instruction &instruction, const Memory &ram) {
  address(code, instruction, ram);
  if (instruction.kind == Kind::bit_branch) {
    bit_branch(code, instruction, ram);
    return;
  }
  if (instruction.kind == Kind::memory_unary &&
      (instruction.operation == RegisterOp::clear || instruction.operation == RegisterOp::test)) {
    single_access(code, instruction, ram);
    return;
  }
  const auto plain = plain_ram(ram);
  if (plain.first == 0 && plain.bytes.size() == address_bytes) {
    known_ram(code, instruction, plain);
    return;
  }
  for (const auto region : ram.regions()) {
    if (region.bytes.empty() || !region.writable) { continue; }
    const auto refused = admit_ram(code, region, {});
    known_ram(code, instruction, region);
    for (const auto position : refused) { code.bind(position); }
  }
  external(code, instruction, ram);
}
}
