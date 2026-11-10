// SPDX-License-Identifier: GPL-3.0-only
#include "accumulator_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "decimal_model.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace cpu12::test::accumulator {
namespace {
constexpr unsigned qualified_flags = 0xfd;

void unsupported(ObservedCore &core, const Registers before) {
  const auto native = core.statistics().native_instructions;
  const auto result = core.run_instruction(decimal_code);
  require(result.retired == 0 && result.fault == Fault::unsupported_state &&
              result.pc == before.pc && result.opcode == decimal_code[0] &&
              result.value == actual_input(before) && result.address == 0 &&
              result.access_size == 0 && core.registers() == before &&
              core.statistics().native_instructions == native,
          "Unqualified DAA input reports actual A/C/H and PC without CPU mutation, fake memory "
          "access or retirement");
  ++unqualified;
}

void table_domain(ObservedCore &core) {
  const auto accepted = checked;
  const auto refused = unqualified;
  constexpr unsigned preserved_b = 0x5a;
  for (unsigned a = 0; a <= byte_mask; ++a) {
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      auto before = seed;
      before.d = static_cast<std::uint16_t>((a << byte_bits) | preserved_b);
      before.ccr = static_cast<Byte>(ccr);
      core.set_registers(before);
      const auto selected = rule(before);
      if (selected) {
        const auto adjusted = (a + selected->correction) % (byte_mask + 1U);
        retired(core,
                decimal_code,
                before,
                decimal_result(before, {.a = adjusted, .carry = selected->result_carry}),
                qualified_flags);
      } else {
        unsupported(core, before);
      }
    }
  }
  constexpr unsigned legal_states = 24576;
  constexpr unsigned uncovered_states = 40960;
  require(checked - accepted == legal_states && unqualified - refused == uncovered_states,
          "Nine independent manual rows cover384 A/C/H triples;640 unqualified triples remain "
          "visible for all other flags");
}

unsigned packed_decimal(const unsigned value) {
  return ((value / 10U) * 16U) + (value % 10U);
}

void bcd_sum(ObservedCore &core, const unsigned left, const unsigned right, const unsigned carry) {
  constexpr unsigned decimal_range = 100;
  constexpr unsigned preserved_b = 0x5a;
  auto before = seed;
  before.d = static_cast<std::uint16_t>((packed_decimal(left) << byte_bits) | preserved_b);
  constexpr unsigned upper_flags = 0xa4;
  before.ccr = static_cast<Byte>(upper_flags | carry);
  core.set_registers(before);
  const auto operand = packed_decimal(right);
  const std::array<Byte, 2> adc{0x89, static_cast<Byte>(operand)};
  const auto raw = arithmetic({.operation = Operation::add, .before = before},
                              {.left = packed_decimal(left), .right = operand, .carry = carry});
  retired(core, adc, before, raw);
  const auto decimal = left + right + carry;
  const auto wanted = decimal_result(
      raw, {.a = packed_decimal(decimal % decimal_range), .carry = decimal >= decimal_range});
  retired(core, decimal_code, raw, wanted, qualified_flags);
}

void aba_sum(ObservedCore &core, const unsigned left, const unsigned right) {
  constexpr unsigned decimal_range = 100;
  auto before = seed;
  before.d =
      static_cast<std::uint16_t>((packed_decimal(left) << byte_bits) | packed_decimal(right));
  core.set_registers(before);
  const auto raw = arithmetic({.operation = Operation::add, .before = before},
                              {.left = packed_decimal(left), .right = packed_decimal(right)});
  retired(core, encode(Operation::add), before, raw);
  const auto sum = left + right;
  retired(core,
          decimal_code,
          raw,
          decimal_result(raw,
                         {.a = packed_decimal(sum % decimal_range), .carry = sum >= decimal_range}),
          qualified_flags);
}

void decimal_sums(ObservedCore &core) {
  constexpr unsigned maximum = 99;
  for (unsigned left = 0; left <= maximum; ++left) {
    for (unsigned right = 0; right <= maximum; ++right) {
      aba_sum(core, left, right);
      bcd_sum(core, left, right, 0);
      bcd_sum(core, left, right, 1);
    }
  }
}

struct Read {
  unsigned calls = 0;

  static bool refused(void *owner, std::uint16_t /*address*/, Byte &value) noexcept {
    auto &read = *static_cast<Read *>(owner);
    ++read.calls;
    constexpr Byte returned = 0x7d;
    value = returned;
    return false;
  }
};

void fault_boundary() {
  Read read;
  Io io{.owner = &read, .read = Read::refused};
  ObservedCore core(Memory::external(), &io);
  core.set_registers(seed);
  constexpr std::array<Byte, 3> old_read{0xb6, 0x12, 0x34};
  const auto old = core.run_instruction(old_read);
  constexpr unsigned old_address = 0x1234;
  require(old.fault == Fault::memory_read && old.address == old_address && old.access_size == 1 &&
              read.calls == 1,
          "Boundary fixture first establishes genuine prior memory fault metadata");
  auto before = seed;
  constexpr std::uint16_t invalid_data = 0x955a;
  before.d = invalid_data;
  before.ccr = half_flag;
  core.set_registers(before);
  unsupported(core, before);
  const auto repeated = core.run_instruction(decimal_code);
  require(
      repeated.fault == Fault::unsupported_state && repeated.value == actual_input(before) &&
          repeated.address == 0 && repeated.access_size == 0 && read.calls == 1 &&
          core.registers() == before,
      "Unqualified state clears stale memory metadata and remains latched without callback replay");
  constexpr Byte mask = 0xde;
  before.ccr &= mask;
  core.set_registers(before);
  const auto selected = rule(before);
  if (!selected) {
    throw std::runtime_error("Explicit fresh registers must provide a documented DAA input");
  }
  const auto recovered = core.run_instruction(decimal_code);
  const auto expected = decimal_result(
      before,
      {.a = (static_cast<unsigned>(before.d >> byte_bits) + selected->correction) & byte_mask,
       .carry = selected->result_carry});
  require(
      recovered.retired == 1 && recovered.fault == Fault::none && core.registers() == expected &&
          read.calls == 1,
      "Explicit fresh boundary recovers the cached native operation without a peripheral access");
}
}

void decimal_cases(ObservedCore &core) {
  table_domain(core);
  decimal_sums(core);
  fault_boundary();
}
}
