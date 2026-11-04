// SPDX-License-Identifier: GPL-3.0-only
#include "jit/emitter.hpp"

#include "cpu12/core.hpp"
#include "instruction.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::detail {
namespace {
constexpr auto pc_offset = static_cast<std::uint8_t>(offsetof(Registers, pc));
constexpr auto d_offset = static_cast<std::uint8_t>(offsetof(Registers, d));
constexpr auto ccr_offset = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr unsigned byte_mask = 0xff;
constexpr unsigned byte_bits = 8;
static_assert(offsetof(Registers, ccr) < 128);
// Instruction templates, in Intel byte order. Only marked immediates/offsets
// are supplied separately; all generated register accesses use the one state.
constexpr std::array<std::uint8_t, 4> entry{0xf3, 0x0f, 0x1e, 0xfa};
constexpr std::array<std::uint8_t, 4> pc_add{0x66, 0x81, 0x47, pc_offset};
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
  load_al = 0xb0,
  add_al = 0x04,
  sub_al = 0x2c,
  cmp_al = 0x3c,
  and_al = 0x24,
  or_al = 0x0c,
  xor_al = 0x34,
  test_al = 0xa8,
  jz = 0x74,
  jnz = 0x75
};

class Code {
public:
  std::vector<std::uint8_t> bytes;

  void raw(const std::span<const std::uint8_t> values) {
    bytes.insert(bytes.end(), values.begin(), values.end());
  }

  void byte(const std::uint8_t value) { bytes.push_back(value); }

  void advance(const int delta) {
    const auto value = static_cast<std::uint16_t>(delta);
    raw(pc_add);
    byte(static_cast<std::uint8_t>(value & byte_mask));
    byte(static_cast<std::uint8_t>(value >> byte_bits));
  }
};

struct FlagPolicy {
  bool carry = false, overflow = false, half = false;
};

void flags(Code &code, const FlagPolicy policy) {
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
  code.raw(store_ccr);
}

void arithmetic(Code &code, const Instruction &instruction) {
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
    case Kind::nop:
    case Kind::branch:
    default: throw std::logic_error("Invalid native arithmetic kind");
  }
  code.byte(opcode);
  code.byte(instruction.operand);
  if (instruction.kind == Kind::load) { code.raw(test_accumulator); }
  if (store) {
    code.raw(store_accumulator);
    code.byte(offset);
  }
  flags(code, policy);
  code.advance(instruction.length);
  code.byte(ret);
}

void branch(Code &code, const Instruction &instruction) {
  constexpr unsigned branch_condition_mask = 15;
  const auto condition = instruction.opcode & branch_condition_mask;
  const auto delta =
      instruction.length + static_cast<int>(std::bit_cast<std::int8_t>(instruction.operand));
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

std::vector<std::uint8_t> emit(const Instruction &instruction) {
  Code code;
  code.raw(entry);
  if (instruction.kind == Kind::branch) {
    branch(code, instruction);
  } else if (instruction.kind == Kind::nop) {
    code.advance(1);
    code.byte(ret);
  } else {
    arithmetic(code, instruction);
  }
  return code.bytes;
}
}
