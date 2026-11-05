// SPDX-License-Identifier: GPL-3.0-only
#include "jit/indexed.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/emission.hpp"
#include "memory_access.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cpu12::detail {
namespace {
static_assert(offsetof(MaskedByteRead, address) == 0 && offsetof(MaskedByteRead, mask) == 2 &&
              sizeof(MaskedByteRead) == 4);
static_assert(sizeof(RunResult) == 16);
constexpr std::array<std::uint8_t, 4> base_offsets{
    static_cast<std::uint8_t>(offsetof(Registers, x)),
    static_cast<std::uint8_t>(offsetof(Registers, y)),
    static_cast<std::uint8_t>(offsetof(Registers, sp)),
    static_cast<std::uint8_t>(offsetof(Registers, pc))};
constexpr std::array<std::uint8_t, 4> word_offsets{
    static_cast<std::uint8_t>(offsetof(Registers, d)),
    static_cast<std::uint8_t>(offsetof(Registers, y)),
    static_cast<std::uint8_t>(offsetof(Registers, x)),
    static_cast<std::uint8_t>(offsetof(Registers, sp))};
constexpr auto d_offset = static_cast<std::uint8_t>(offsetof(Registers, d));
constexpr unsigned word_mask = 0xffff;

// Intel-order templates. Variable register offsets, RAM pointers and values
// are emitted separately. Template selection and patching occur during compilation.
constexpr std::array<std::uint8_t, 4> load_base{0x44, 0x0f, 0xb7, 0x4f};
constexpr std::array<std::uint8_t, 3> add_address{0x41, 0x81, 0xc1};
constexpr std::array<std::uint8_t, 2> load_offset_prefix{0x44, 0x0f};
constexpr std::array<std::uint8_t, 4> pc_accumulator{0x47, 0x8d, 0x4c, 0x11};
constexpr std::array<std::uint8_t, 3> add_accumulator{0x45, 0x01, 0xd1};
constexpr std::array<std::uint8_t, 3> mask_address{0x41, 0x81, 0xe1};
constexpr std::array<std::uint8_t, 1> argument_prefix{0x0f};
constexpr std::array<std::uint8_t, 9> call_enter{
    0x57, 0x56, 0x48, 0x83, 0xec, 0x08, 0x48, 0x89, 0xf7};
constexpr std::array<std::uint8_t, 3> call_address{0x44, 0x89, 0xce};
constexpr std::array<std::uint8_t, 2> absolute_pointer{0x49, 0xbb};
constexpr std::array<std::uint8_t, 3> call_absolute{0x41, 0xff, 0xd3};
constexpr std::array<std::uint8_t, 6> call_leave{0x48, 0x83, 0xc4, 0x08, 0x5e, 0x5f};
constexpr std::array<std::uint8_t, 2> write_status{0x85, 0xc0};
constexpr std::array<std::uint8_t, 2> read_value{0x89, 0xc1};
constexpr std::array<std::uint8_t, 4> next_address{0x45, 0x8d, 0x41, 0x01};
constexpr std::array<std::uint8_t, 3> mask_low{0x41, 0x81, 0xe0};
constexpr std::array<std::uint8_t, 3> move_address{0x45, 0x89, 0xca};
constexpr std::array<std::uint8_t, 2> immediate_prefix{0x41, 0x81};
constexpr std::array<std::uint8_t, 2> outside_jump{0x0f, 0x83};
constexpr std::array<std::uint8_t, 5> high_value{0x89, 0xc2, 0xc1, 0xea, 0x08};
constexpr std::array<std::uint8_t, 3> high_write_prefix{0x43, 0x88, 0x14};
constexpr std::array<std::uint8_t, 3> byte_write_prefix{0x43, 0x88, 0x04};
constexpr std::array<std::uint8_t, 4> low_write{0x43, 0x88, 0x04, 0x03};
constexpr std::array<std::uint8_t, 4> high_read_prefix{0x43, 0x0f, 0xb6, 0x0c};
constexpr std::array<std::uint8_t, 3> shift_high{0xc1, 0xe1, 0x08};
constexpr std::array<std::uint8_t, 5> low_read{0x47, 0x0f, 0xb6, 0x04, 0x03};
constexpr std::array<std::uint8_t, 3> merge_word{0x44, 0x09, 0xc1};
constexpr std::array<std::uint8_t, 3> pointer_result{0x41, 0x89, 0xc9};
constexpr std::array<std::uint8_t, 3> modify_index{0x66, 0x81, 0x47};
constexpr std::array<std::uint8_t, 2> advance_source{0x66, 0x05};
constexpr std::array<std::uint8_t, 4> store_address{0x66, 0x44, 0x89, 0x4f};

enum Encoding : std::uint8_t {
  byte_extend = 0xb6,
  word_extend = 0xb7,
  offset_operand = 0x57,
  source_operand = 0x47,
  write_argument = 0xd0,
  compare_result = 0x3d,
  subtract_low = 0xe8,
  subtract_high = 0xea,
  compare_low = 0xf8,
  compare_high = 0xfa,
  jump_relative = 0xe9,
  full_index = 0x0b,
  partial_index = 0x13
};

bool replaces_base(const Instruction &instruction) {
  if (instruction.kind != Kind::effective_address &&
      (instruction.kind != Kind::load || !instruction.word)) {
    return false;
  }
  return word_offsets[static_cast<unsigned>(instruction.target)] ==
         base_offsets[static_cast<unsigned>(instruction.indexed.base)];
}

void update_index(Code &code, const Instruction &instruction) {
  if (instruction.indexed.update == 0 || replaces_base(instruction)) { return; }
  code.raw(modify_index);
  code.byte(base_offsets[static_cast<unsigned>(instruction.indexed.base)]);
  code.u16(static_cast<std::uint16_t>(instruction.indexed.update));
}

void effective_address(Code &code, const Instruction &instruction) {
  const auto &indexed = instruction.indexed;
  code.raw(load_base);
  code.byte(base_offsets[static_cast<unsigned>(indexed.base)]);
  const unsigned pc_adjustment = indexed.base == IndexBase::pc ? instruction.length : 0;
  if (indexed.offset == IndexOffset::constant) {
    const auto displacement = (indexed.displacement + pc_adjustment) & word_mask;
    if (displacement == 0) { return; }
    code.raw(add_address);
    code.u32(displacement);
  } else {
    const bool word = indexed.offset == IndexOffset::d;
    code.raw(load_offset_prefix);
    code.byte(word ? word_extend : byte_extend);
    code.byte(offset_operand);
    code.byte(static_cast<std::uint8_t>(d_offset + (indexed.offset == IndexOffset::a ? 1 : 0)));
    if (pc_adjustment != 0) {
      code.raw(pc_accumulator);
      code.byte(static_cast<std::uint8_t>(pc_adjustment));
    } else {
      code.raw(add_accumulator);
    }
  }
  code.raw(mask_address);
  code.u32(word_mask);
}

void bus_call(Code &code, const Access access) {
  if (access.write) {
    code.raw(argument_prefix);
    code.byte(access.word ? word_extend : byte_extend);
    code.byte(write_argument); // movzx edx, ax/al
  }
  // Save state/context, align SysV stack, then pass context and actual r9 address.
  code.raw(call_enter);
  code.raw(call_address);
  if (access.selected && access.mask != 0) {
    // SysV packs MaskedByteRead into ESI: address16 followed by mask8.
    constexpr std::array<std::uint8_t, 2> pack{0x81, 0xce};
    constexpr unsigned mask_shift = 16;
    code.raw(pack);
    code.u32(static_cast<std::uint32_t>(access.mask) << mask_shift);
  }
  code.raw(absolute_pointer);
  if (access.write) {
    code.u64(access.word ? reinterpret_cast<std::uintptr_t>(&write_word)
                         : reinterpret_cast<std::uintptr_t>(&write_byte));
  } else if (access.selected) {
    code.u64(reinterpret_cast<std::uintptr_t>(&read_masked_byte));
  } else {
    code.u64(access.word ? reinterpret_cast<std::uintptr_t>(&read_word)
                         : reinterpret_cast<std::uintptr_t>(&read_byte));
  }
  code.raw(call_absolute);
  code.raw(call_leave);
  if (access.write) {
    code.raw(write_status);
  } else {
    code.byte(compare_result); // cmp eax, failed_read/failed_word_read
    code.u32(access.word ? failed_word_read : failed_read);
  }
  constexpr std::uint8_t accepted = 0x75;
  code.byte(accepted);
  code.byte(code.return_size());
  code.return_(NativeExit::fault);
  if (!access.write) { code.raw(read_value); }
}

void offset(Code &code, const Ram ram, const bool low) {
  if (low) {
    code.raw(next_address); // lea r8d, [r9+1]
    code.raw(mask_low);
    code.u32(word_mask);
  } else {
    code.raw(move_address); // mov r10d, r9d
  }
  if (ram.first != 0) {
    code.raw(immediate_prefix);
    code.byte(low ? subtract_low : subtract_high);
    code.u32(ram.first);
  }
}

std::size_t outside(Code &code, const Ram ram, const bool low) {
  offset(code, ram, low);
  code.raw(immediate_prefix);
  code.byte(low ? compare_low : compare_high);
  code.u32(static_cast<std::uint32_t>(ram.bytes.size()));
  code.raw(outside_jump); // jae actual owner
  return code.relative32();
}

void native_access(Code &code, const Ram ram, const Access access) {
  const auto high_sib = static_cast<std::uint8_t>(
      ram.first == 0 && ram.bytes.size() == address_bytes ? full_index : partial_index);
  code.raw(absolute_pointer);
  code.u64(reinterpret_cast<std::uintptr_t>(ram.bytes.data()));
  if (access.write) {
    if (access.word) {
      code.raw(high_value);
      code.raw(high_write_prefix);
      code.byte(high_sib);
      code.raw(low_write);
    } else {
      code.raw(byte_write_prefix);
      code.byte(high_sib);
    }
  } else {
    code.raw(high_read_prefix);
    code.byte(high_sib);
    if (access.word) {
      code.raw(shift_high);
      code.raw(low_read);
      code.raw(merge_word);
    }
  }
}

void mark_known(Code &code, const Ram ram, const Access access) {
  if (!access.write || ram.known.empty()) { return; }
  code.raw(absolute_pointer);
  code.u64(reinterpret_cast<std::uintptr_t>(ram.known.data()));
  constexpr std::array<std::uint8_t, 5> high{0x43, 0xc6, 0x04, 0x13, 1};
  constexpr std::array<std::uint8_t, 5> low{0x43, 0xc6, 0x04, 0x03, 1};
  code.raw(high);
  if (access.word) { code.raw(low); }
}

std::size_t unknown(Code &code, const bool low) {
  constexpr std::array<std::uint8_t, 3> compare{0x43, 0x80, 0x3c};
  constexpr std::array<std::uint8_t, 2> zero{0x0f, 0x84};
  code.raw(compare);
  code.byte(low ? 0x03 : partial_index);
  code.byte(0);
  code.raw(zero);
  return code.relative32();
}
}

std::vector<std::size_t> admit_ram(Code &code, const Ram ram, const Access access) {
  std::vector<std::size_t> refused{outside(code, ram, false)};
  if (access.word) { refused.push_back(outside(code, ram, true)); }
  if (!access.write && !ram.known.empty()) {
    code.raw(absolute_pointer);
    code.u64(reinterpret_cast<std::uintptr_t>(ram.known.data()));
    refused.push_back(unknown(code, false));
    if (access.word) { refused.push_back(unknown(code, true)); }
  }
  return refused;
}

void native_data_access(Code &code, const Ram ram, const Access access) {
  native_access(code, ram, access);
  mark_known(code, ram, access);
}

void data_access(Code &code, const Memory &memory, const Access access) {
  const auto plain = plain_ram(memory);
  if (plain.first == 0 && plain.bytes.size() == address_bytes) {
    if (access.word) { offset(code, plain, true); }
    native_access(code, plain, access);
    return;
  }
  std::vector<std::size_t> done;
  for (const auto ram : memory.regions()) {
    if (ram.bytes.empty() || (access.write && !ram.writable)) { continue; }
    const auto refused = admit_ram(code, ram, access);
    native_data_access(code, ram, access);
    code.byte(jump_relative);
    done.push_back(code.relative32());
    for (const auto position : refused) { code.bind(position); }
  }
  bus_call(code, access);
  for (const auto position : done) { code.bind(position); }
}

void indexed_address(Code &code, const Instruction &instruction, const Memory &ram) {
  effective_address(code, instruction);
  if (instruction.indexed.indirect) {
    data_access(code, ram, {.word = true});
    code.raw(pointer_result); // mov r9d, ecx
  }
}

void indexed_commit(Code &code, const Instruction &instruction) {
  update_index(code, instruction);
}

void indexed_read(Code &code, const Instruction &instruction, const Memory &ram) {
  indexed_address(code, instruction, ram);
  data_access(code, ram, {.word = instruction.word});
  // A refused access has already returned before any CPU mutation. Successful
  // auto-update precedes the operation, whose register destination may replace it.
  update_index(code, instruction);
}

void indexed_write(Code &code, const Instruction &instruction, const Memory &ram) {
  indexed_address(code, instruction, ram);
  // The indirect pointer access can use a real ABI callback, so load the source
  // only after resolving it. CPU registers are unchanged until accepted access.
  code.raw(argument_prefix);
  code.byte(instruction.word ? word_extend : byte_extend);
  code.byte(source_operand);
  code.byte(instruction.word
                ? word_offsets[static_cast<unsigned>(instruction.target)]
                : static_cast<std::uint8_t>(d_offset + (instruction.accumulator_b ? 0 : 1)));
  if (instruction.indexed.update != 0 && instruction.word &&
      word_offsets[static_cast<unsigned>(instruction.target)] ==
          base_offsets[static_cast<unsigned>(instruction.indexed.base)]) {
    // Match the adjusted architectural source without exposing a speculative
    // CPU update to an external callback that may still refuse the write.
    code.raw(advance_source);
    code.u16(static_cast<std::uint16_t>(instruction.indexed.update));
  }
  data_access(code, ram, {.word = instruction.word, .write = true});
  update_index(code, instruction);
}

void jump(Code &code, const Instruction &instruction, const Memory &ram) {
  constexpr auto pc_offset = static_cast<std::uint8_t>(offsetof(Registers, pc));
  if (instruction.addressing == Addressing::extended) {
    constexpr std::array<std::uint8_t, 3> store_immediate{0x66, 0xc7, 0x47};
    code.raw(store_immediate);
    code.byte(pc_offset);
    code.u16(instruction.address);
  } else {
    indexed_address(code, instruction, ram);
    update_index(code, instruction);
    code.raw(store_address);
    code.byte(pc_offset);
  }
  code.return_();
}

void indexed_effective_address(Code &code, const Instruction &instruction) {
  effective_address(code, instruction);
  update_index(code, instruction);
  code.raw(store_address);
  code.byte(word_offsets[static_cast<unsigned>(instruction.target)]);
  code.advance(instruction.length);
  code.return_();
}
}
