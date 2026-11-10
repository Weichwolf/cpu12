// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"
#include "isa.hpp"
#include "reference.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace cpu12::test::prepared_indexed {
constexpr Address entry = 0x4000;
constexpr std::size_t stride = 6, batch_size = 12;
constexpr unsigned byte_bits = 8, byte_mask = 0xff, high_mask = 0xff00;
constexpr unsigned address_delta = 0x20, accumulator_bit = 0x40;
constexpr std::array byte_operations{
    isa::Opcode::ldaa_indexed, isa::Opcode::ldab_indexed, isa::Opcode::adda_indexed,
    isa::Opcode::addb_indexed, isa::Opcode::adca_indexed, isa::Opcode::adcb_indexed,
    isa::Opcode::suba_indexed, isa::Opcode::subb_indexed, isa::Opcode::sbca_indexed,
    isa::Opcode::sbcb_indexed, isa::Opcode::cmpa_indexed, isa::Opcode::cmpb_indexed,
    isa::Opcode::anda_indexed, isa::Opcode::andb_indexed, isa::Opcode::oraa_indexed,
    isa::Opcode::orab_indexed, isa::Opcode::eora_indexed, isa::Opcode::eorb_indexed,
    isa::Opcode::bita_indexed, isa::Opcode::bitb_indexed, isa::Opcode::staa_indexed,
    isa::Opcode::stab_indexed};

struct Operation {
  Byte opcode;
  bool word;
  std::size_t index;

  [[nodiscard]] bool writes() const {
    return word ? indexed::words[index].function == indexed::Function::store
                : opcode == static_cast<Byte>(isa::Opcode::staa_indexed) ||
                      opcode == static_cast<Byte>(isa::Opcode::stab_indexed);
  }
};

inline std::vector<Operation> operations() {
  std::vector<Operation> result;
  result.reserve(byte_operations.size() + indexed::words.size());
  for (std::size_t index = 0; index < byte_operations.size(); ++index) {
    result.push_back(
        {.opcode = static_cast<Byte>(byte_operations[index]), .word = false, .index = index});
  }
  for (std::size_t index = 0; index < indexed::words.size(); ++index) {
    result.push_back({.opcode = indexed::words[index].opcode, .word = true, .index = index});
  }
  return result;
}

struct Form {
  indexed::Form address;
  int update = 0;
};

inline std::vector<Form> forms() {
  std::vector<Form> result;
  for (const auto form : indexed::forms()) { result.push_back({.address = form}); }
  constexpr std::array<indexed::Field, 3> bases{&Registers::x, &Registers::y, &Registers::sp};
  constexpr unsigned base_shift = 6;
  constexpr unsigned post = 0x10;
  for (unsigned base = 0; base < bases.size(); ++base) {
    for (int update = -8; update <= 8; ++update) {
      if (update == 0) { continue; }
      const auto raw = static_cast<unsigned>(update > 0 ? update - 1 : update + 16);
      for (const bool before : {false, true}) {
        result.push_back(
            {.address = {.base = bases[base],
                         .displacement = before ? update : 0,
                         .postbyte = static_cast<Byte>((base << base_shift) | address_delta |
                                                       (before ? 0U : post) | raw)},
             .update = update});
      }
    }
  }
  return result;
}

inline void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

inline Expected with_carry(const Input point) {
  constexpr unsigned add_opcode = 0x89;
  const bool add = (point.opcode & (byte_mask ^ accumulator_bit)) == add_opcode;
  const unsigned cin = point.ccr & carry;
  const int raw = static_cast<int>(point.left) + (add ? static_cast<int>(point.right + cin)
                                                      : -static_cast<int>(point.right + cin));
  const int mathematical =
      signed_byte(point.left) + (add ? signed_byte(point.right) + static_cast<int>(cin)
                                     : -signed_byte(point.right) - static_cast<int>(cin));
  const auto value = static_cast<unsigned>(raw + static_cast<int>(values)) % values;
  const bool carry_out = add ? raw >= static_cast<int>(values) : raw < 0;
  const auto bound = static_cast<int>(sign_bit);
  const unsigned changed = nzvc | (add ? half : 0U);
  const unsigned h = add && (point.left % 16) + (point.right % 16) + cin >= 16 ? half : 0U;
  const auto flags = (carry_out ? carry : 0U) |
                     (mathematical < -bound || mathematical >= bound ? overflow : 0U) |
                     (value == 0 ? zero : 0U) | (value >= sign_bit ? negative : 0U) | h;
  return {.value = value, .ccr = (point.ccr & (byte_mask ^ changed)) | flags};
}

inline Registers
byte_expected(Registers before, const Operation operation, const unsigned operand) {
  const bool b = operation.writes()
                     ? operation.opcode == static_cast<Byte>(isa::Opcode::stab_indexed)
                     : (operation.opcode & accumulator_bit) != 0;
  const auto left = b ? before.d & byte_mask : before.d >> byte_bits;
  Expected result{.value = left, .ccr = 0};
  if (operation.writes()) {
    constexpr unsigned retained = 0xf1;
    result.ccr =
        (before.ccr & retained) | (left == 0 ? zero : 0U) | (left >= sign_bit ? negative : 0U);
  } else {
    const auto opcode = static_cast<unsigned>(operation.opcode) - address_delta;
    const Input point{.opcode = opcode, .left = left, .right = operand, .ccr = before.ccr};
    constexpr unsigned adc = 0x89;
    constexpr unsigned sbc = 0x82;
    const auto family = opcode & (byte_mask ^ accumulator_bit);
    result = family == adc || family == sbc ? with_carry(point) : expected(point);
    if (result.store) {
      before.d = static_cast<Address>(b ? (before.d & high_mask) | result.value
                                        : (before.d & byte_mask) | (result.value << byte_bits));
    }
  }
  before.ccr = static_cast<Byte>(result.ccr);
  return before;
}

std::size_t matrix(std::size_t part);
void boundaries();
}
