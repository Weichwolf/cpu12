// SPDX-License-Identifier: GPL-3.0-only
#include "jit/stack.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/indexed.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace cpu12::detail {
namespace {
constexpr auto pc_offset = static_cast<std::uint8_t>(offsetof(Registers, pc));
constexpr auto sp_offset = static_cast<std::uint8_t>(offsetof(Registers, sp));
constexpr auto ccr_offset = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr auto d_offset = static_cast<std::uint8_t>(offsetof(Registers, d));
constexpr std::array<std::uint8_t, 7> offsets{static_cast<std::uint8_t>(offsetof(Registers, x)),
                                              static_cast<std::uint8_t>(offsetof(Registers, y)),
                                              static_cast<std::uint8_t>(d_offset + 1),
                                              d_offset,
                                              ccr_offset,
                                              d_offset,
                                              pc_offset};
constexpr std::array<std::uint8_t, 3> load_word{0x0f, 0xb7, 0x47};
constexpr std::array<std::uint8_t, 3> load_byte{0x0f, 0xb6, 0x47};
constexpr std::array<std::uint8_t, 3> store_word{0x66, 0x89, 0x4f};
constexpr std::array<std::uint8_t, 2> store_byte{0x88, 0x4f};
constexpr std::array<std::uint8_t, 4> stack_change{0x66, 0x81, 0x47, sp_offset};
constexpr std::array<std::uint8_t, 5> save_target{0x66, 0x44, 0x89, 0x0c, 0x24};
constexpr std::array<std::uint8_t, 5> restore_target{0x66, 0x44, 0x8b, 0x0c, 0x24};
constexpr std::array<std::uint8_t, 3> register_target{0x41, 0x89, 0xc9};
constexpr std::array<std::uint8_t, 4> store_target{0x66, 0x44, 0x89, 0x4f};
constexpr std::array<std::uint8_t, 4> store_immediate{0x66, 0xc7, 0x47, pc_offset};
constexpr std::array<std::uint8_t, 2> add_ax{0x66, 0x05};

Instruction stack_address(const int displacement) {
  return {
      .addressing = Addressing::indexed,
      .indexed = {.base = IndexBase::sp, .displacement = static_cast<std::uint16_t>(displacement)}};
}

void change_sp(Code &code, const int amount) {
  code.raw(stack_change);
  code.u16(static_cast<std::uint16_t>(amount));
}

void pull(Code &code, const Instruction &instruction) {
  if (instruction.stack_target == StackTarget::ccr) {
    // X can remain set or clear, but PULC cannot raise a previously clear X.
    constexpr std::uint8_t retain_x = 0xbf;
    constexpr std::array<std::uint8_t, 2> mask_allowed{0x0c, retain_x}; // or al, $bf
    constexpr std::array<std::uint8_t, 2> apply_mask{0x20, 0xc1};       // and cl, al
    code.raw(load_byte);
    code.byte(ccr_offset);
    code.raw(mask_allowed);
    code.raw(apply_mask);
  }
  if (instruction.word) {
    code.raw(store_word);
  } else {
    code.raw(store_byte);
  }
  code.byte(offsets[static_cast<unsigned>(instruction.stack_target)]);
}
}

void stack_instruction(Code &code, const Instruction &instruction, const Ram ram) {
  const bool push = instruction.kind == Kind::push;
  const int width = instruction.word ? 2 : 1;
  indexed_address(code, stack_address(push ? -width : 0), ram);
  if (push) {
    if (instruction.word) {
      code.raw(load_word);
    } else {
      code.raw(load_byte);
    }
    code.byte(offsets[static_cast<unsigned>(instruction.stack_target)]);
  }
  data_access(code, ram, {.word = instruction.word, .write = push});
  if (!push) { pull(code, instruction); }
  change_sp(code, push ? -width : width);
  if (instruction.kind != Kind::return_subroutine) { code.advance(instruction.length); }
  code.return_();
}

void subroutine_call(Code &code, const Instruction &instruction, const Ram ram) {
  const bool indexed = instruction.addressing == Addressing::indexed;
  const bool indirect = indexed && instruction.indexed.indirect;
  const bool framed = indirect && (ram.first != 0 || ram.bytes.size() != address_bytes);
  if (indirect) {
    // Resolve the actual pointer before a possibly overlapping return-stack write.
    // A refused pointer read returns before this temporary host frame exists.
    indexed_address(code, instruction, ram);
    if (framed) {
      code.frame();
      code.raw(save_target);
    }
  }
  const int auto_sp =
      indexed && instruction.indexed.base == IndexBase::sp ? instruction.indexed.update : 0;
  indexed_address(code, stack_address(auto_sp - 2), ram);
  code.raw(load_word);
  code.byte(pc_offset);
  code.raw(add_ax);
  code.u16(instruction.length);
  data_access(code, ram, {.word = true, .write = true});
  if (indexed) {
    if (indirect) {
      if (framed) {
        code.raw(restore_target);
      } else {
        // Full-RAM stores cannot call the owner and retain ECX's resolved pointer.
        code.raw(register_target);
      }
    } else {
      indexed_address(code, instruction, ram);
    }
    indexed_commit(code, instruction);
  }
  change_sp(code, -2);
  if (indexed) {
    code.raw(store_target);
    code.byte(pc_offset);
  } else if (instruction.addressing == Addressing::relative) {
    code.advance(instruction.length +
                 static_cast<int>(std::bit_cast<std::int16_t>(instruction.operand)));
  } else {
    code.raw(store_immediate);
    code.u16(instruction.address);
  }
  code.return_();
}
}
