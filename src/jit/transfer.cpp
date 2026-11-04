// SPDX-License-Identifier: GPL-3.0-only
#include "jit/transfer.hpp"

#include "cpu12/core.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/flags.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace cpu12::detail {
namespace {
constexpr auto d_offset = static_cast<std::uint8_t>(offsetof(Registers, d));
constexpr auto ccr_offset = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr unsigned ccr_selector = 2;

struct Register {
  std::uint8_t offset = 0;
  bool word = false;
};
enum class Value : std::uint8_t { source, destination };

Register descriptor(const unsigned selector) {
  switch (selector) {
    case 0: return {.offset = static_cast<std::uint8_t>(d_offset + 1)};
    case 1: return {.offset = d_offset};
    case ccr_selector: return {.offset = ccr_offset};
    case 4: return {.offset = d_offset, .word = true};
    case 5: return {.offset = static_cast<std::uint8_t>(offsetof(Registers, x)), .word = true};
    case 6: return {.offset = static_cast<std::uint8_t>(offsetof(Registers, y)), .word = true};
    case 7: return {.offset = static_cast<std::uint8_t>(offsetof(Registers, sp)), .word = true};
    default: throw std::logic_error("Unsupported CPU12 transfer register");
  }
}

void load(Code &code, const Register reg, const Value value, const bool sign_extend) {
  constexpr std::uint8_t extend_prefix = 0x0f;
  constexpr std::uint8_t signed_byte = 0xbe;
  constexpr std::uint8_t unsigned_byte = 0xb6;
  constexpr std::uint8_t unsigned_word = 0xb7;
  constexpr std::uint8_t to_ax = 0x47;
  constexpr std::uint8_t to_cx = 0x4f;
  code.byte(extend_prefix);
  if (reg.word) {
    code.byte(unsigned_word);
  } else {
    code.byte(sign_extend ? signed_byte : unsigned_byte);
  }
  code.byte(value == Value::source ? to_ax : to_cx);
  code.byte(reg.offset);
}

void ccr_mask(Code &code, const Value value) {
  constexpr std::array<std::uint8_t, 4> old_ccr{0x0f, 0xb6, 0x57, ccr_offset};
  constexpr std::array<std::uint8_t, 3> allowed_bits{0x80, 0xca, 0xbf};
  constexpr std::array<std::uint8_t, 2> mask_al{0x20, 0xd0};
  constexpr std::array<std::uint8_t, 2> mask_cl{0x20, 0xd1};
  code.raw(old_ccr);
  code.raw(allowed_bits);
  if (value == Value::source) {
    code.raw(mask_al);
  } else {
    code.raw(mask_cl);
  }
}

void store(Code &code, const Register reg, const Value value) {
  if (reg.offset == ccr_offset) { ccr_mask(code, value); }
  constexpr std::uint8_t word_prefix = 0x66;
  constexpr std::uint8_t byte_move = 0x88;
  constexpr std::uint8_t word_move = 0x89;
  constexpr std::uint8_t from_ax = 0x47;
  constexpr std::uint8_t from_cx = 0x4f;
  if (reg.word) { code.byte(word_prefix); }
  code.byte(reg.word ? word_move : byte_move);
  code.byte(value == Value::source ? from_ax : from_cx);
  code.byte(reg.offset);
}
}

void transfer_instruction(Code &code, const Instruction &instruction) {
  constexpr unsigned source_shift = 4;
  constexpr unsigned selector_mask = 7;
  constexpr unsigned exchange_mask = 0x80;
  constexpr unsigned prefix = 0x18;
  const auto source_id = (instruction.operand >> source_shift) & selector_mask;
  const auto destination_id = instruction.operand & selector_mask;
  const auto source = descriptor(source_id);
  const auto destination = descriptor(destination_id);
  const bool exchange = (instruction.operand & exchange_mask) != 0;
  load(code, source, Value::source, !exchange && destination.word);
  if (exchange) {
    // Snapshot both operands before either write, including overlapping A/B/D.
    load(code, destination, Value::destination, false);
    if (source.word && !destination.word && destination_id != 0) {
      constexpr std::array<std::uint8_t, 2> fill_high{0x81, 0xc9};
      constexpr unsigned high_byte = 0xff00;
      code.raw(fill_high);
      code.u32(high_byte);
    }
    store(code, source, Value::destination);
  }
  store(code, destination, Value::source);
  if (instruction.opcode == prefix) {
    constexpr std::array<std::uint8_t, 2> test_result{0x84, 0xc0};
    code.raw(test_result);
    flags(code, {});
  }
  code.advance(instruction.length);
  code.return_();
}
}
