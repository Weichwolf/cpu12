// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "multiply_divide_model.hpp"

#include <array>
#include <cstdint>

namespace cpu12::test::math {
namespace {
constexpr std::array<unsigned, 14> anchors{
    0, 1, 2, 3, 7, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff};
constexpr std::array<unsigned, 4> flags{0, byte_mask, 0xa5, 0x5a};
constexpr std::array<Operation, 5> operations{
    Operation::idiv, Operation::idivs, Operation::ediv, Operation::edivs, Operation::fdiv};

bool extended(const Operation operation) {
  return operation == Operation::ediv || operation == Operation::edivs;
}

bool signed_operation(const Operation operation) {
  return operation == Operation::idivs || operation == Operation::edivs;
}

Expected zero(const Input point) {
  auto wanted = point.before;
  constexpr unsigned defined_error = 0xf1;
  unsigned defined = defined_error;
  bool undefined_d = false;
  if (point.operation == Operation::idiv || point.operation == Operation::fdiv) {
    wanted.x = word_mask;
    constexpr unsigned retained = 0xf8;
    wanted.ccr =
        static_cast<Byte>((wanted.ccr & retained) | (point.operation == Operation::fdiv ? 3U : 1U));
    defined = byte_mask;
    undefined_d = true;
  } else {
    wanted.ccr = static_cast<Byte>(wanted.ccr | 1U);
  }
  return {.registers = wanted, .defined_ccr = defined, .undefined_d = undefined_d};
}

Expected fraction_overflow(const Input point) {
  auto wanted = point.before;
  wanted.x = word_mask;
  constexpr unsigned retained = 0xf8;
  wanted.ccr = static_cast<Byte>((wanted.ccr & retained) | 2U);
  return {.registers = wanted, .undefined_d = true};
}

struct Result {
  std::int64_t quotient, remainder;
};

Result division(const Input point) {
  const auto operation = point.operation;
  const auto divisor = signed_operation(operation) ? signed_word(point.before.x) : point.before.x;
  std::int64_t numerator = point.before.d;
  if (extended(operation)) {
    const auto raw = (static_cast<std::uint64_t>(point.before.y) << word_bits) | point.before.d;
    constexpr std::uint64_t long_sign = 0x80000000;
    numerator = static_cast<std::int64_t>(raw);
    if (operation == Operation::edivs && raw >= long_sign) {
      numerator -= static_cast<std::int64_t>(long_mask + 1U);
    }
  } else if (operation == Operation::idivs) {
    numerator = signed_word(point.before.d);
  } else if (operation == Operation::fdiv) {
    numerator *= word_mask + 1U;
  }
  const auto quotient = numerator / divisor;
  return {.quotient = quotient, .remainder = numerator - (quotient * divisor)};
}

Expected finite(const Input point) {
  const auto operation = point.operation;
  if (operation == Operation::fdiv && point.before.x <= point.before.d) {
    return fraction_overflow(point);
  }
  const auto result = division(point);
  constexpr std::int64_t signed_min = -32768;
  constexpr std::int64_t signed_max = 32767;
  const bool overflow = signed_operation(operation)
                            ? result.quotient < signed_min || result.quotient > signed_max
                            : result.quotient > word_mask;
  auto wanted = point.before;
  if (extended(operation) && overflow) {
    constexpr unsigned retained = 0xfe;
    constexpr unsigned defined = 0xf3;
    wanted.ccr = static_cast<Byte>((wanted.ccr & retained) | 2U);
    return {.registers = wanted, .defined_ccr = defined};
  }
  const auto quotient = static_cast<std::uint16_t>(result.quotient);
  wanted.d = static_cast<std::uint16_t>(result.remainder);
  if (extended(operation)) {
    wanted.y = quotient;
  } else {
    wanted.x = quotient;
  }
  const bool nz = extended(operation) || operation == Operation::idivs;
  constexpr unsigned retain_upper = 0xf0;
  constexpr unsigned retain_negative = 0xf8;
  constexpr unsigned word_sign = 0x8000;
  const auto n = nz && quotient >= word_sign ? 8U : 0U;
  wanted.ccr = static_cast<Byte>((wanted.ccr & (nz ? retain_upper : retain_negative)) | n |
                                 (quotient == 0 ? 4U : 0U) | (overflow ? 2U : 0U));
  constexpr unsigned defined_overflow = 0xf3;
  return {.registers = wanted, .defined_ccr = overflow ? defined_overflow : byte_mask};
}

void one(Core &core, const Input point) {
  auto expected = point.before.x == 0 ? zero(point) : finite(point);
  expected.registers.pc =
      static_cast<std::uint16_t>(point.before.pc + encode(point.operation).length);
  retired(core, point, expected);
}

void sixteen_bit(Core &core, const Operation operation) {
  constexpr std::uint16_t unchanged_y = 0x5aa5;
  for (const auto divisor : anchors) {
    for (unsigned data = 0; data <= word_mask; ++data) {
      for (const auto ccr : flags) {
        auto before = seed;
        before.d = static_cast<std::uint16_t>(data);
        before.x = static_cast<std::uint16_t>(divisor);
        before.y = unchanged_y;
        before.ccr = static_cast<Byte>(ccr);
        one(core, {.operation = operation, .before = before});
      }
    }
  }
}

struct Scene {
  std::uint16_t high, divisor;
};

void lower_domain(Core &core, const Operation operation) {
  constexpr std::array<Scene, 14> scenes{{{.high = 0, .divisor = 1},
                                          {.high = 0, .divisor = 0x8000},
                                          {.high = 0, .divisor = 0xffff},
                                          {.high = 1, .divisor = 2},
                                          {.high = 1, .divisor = 1},
                                          {.high = 0x7fff, .divisor = 0x8000},
                                          {.high = 0x8000, .divisor = 0x8001},
                                          {.high = 0xffff, .divisor = 0xffff},
                                          {.high = 0xffff, .divisor = 0x8000},
                                          {.high = 0x7fff, .divisor = 0xffff},
                                          {.high = 0x8000, .divisor = 0xffff},
                                          {.high = 0xffff, .divisor = 1},
                                          {.high = 0, .divisor = 0},
                                          {.high = 0x8000, .divisor = 0}}};
  for (const auto scene : scenes) {
    for (unsigned low = 0; low <= word_mask; ++low) {
      for (const auto ccr : flags) {
        auto before = seed;
        before.y = scene.high;
        before.d = static_cast<std::uint16_t>(low);
        before.x = scene.divisor;
        before.ccr = static_cast<Byte>(ccr);
        one(core, {.operation = operation, .before = before});
      }
    }
  }
}

void upper_domain(Core &core, const Operation operation) {
  constexpr std::array<unsigned, 4> words{0, 0x7fff, 0x8000, 0xffff};
  constexpr std::array<unsigned, 4> divisors{1, 0x7fff, 0x8000, 0xffff};
  for (unsigned high = 0; high <= word_mask; ++high) {
    for (const auto low : words) {
      for (const auto divisor : divisors) {
        auto before = seed;
        before.y = static_cast<std::uint16_t>(high);
        before.d = static_cast<std::uint16_t>(low);
        before.x = static_cast<std::uint16_t>(divisor);
        one(core, {.operation = operation, .before = before});
      }
    }
  }
}

void divisor_domain(Core &core, const Operation operation) {
  constexpr std::array<std::uint32_t, 14> dividends{0,
                                                    1,
                                                    0x7fff,
                                                    0x8000,
                                                    0xffff,
                                                    0x10000,
                                                    0x7fff7fff,
                                                    0x7fffffff,
                                                    0x80000000,
                                                    0x80008000,
                                                    0xffff0000,
                                                    0xffff7fff,
                                                    0xffff8000,
                                                    0xffffffff};
  for (unsigned divisor = 0; divisor <= word_mask; ++divisor) {
    for (const auto dividend : dividends) {
      auto before = seed;
      before.y = static_cast<std::uint16_t>(dividend >> word_bits);
      before.d = static_cast<std::uint16_t>(dividend);
      before.x = static_cast<std::uint16_t>(divisor);
      one(core, {.operation = operation, .before = before});
    }
  }
}

void directed_flags(Core &core) {
  constexpr std::array<Registers, 8> cases{{{.pc = 0xffff, .d = 0, .x = 0, .y = 0},
                                            {.pc = 0, .d = 0xffff, .x = 0, .y = 0x8000},
                                            {.pc = 0xfffe, .d = 0x8000, .x = 0xffff, .y = 0xffff},
                                            {.pc = 1, .d = 0, .x = 0xffff, .y = 0x8000},
                                            {.pc = 0xffff, .d = 0xffff, .x = 1, .y = 0x7fff},
                                            {.pc = 0, .d = 1, .x = 2, .y = 0},
                                            {.pc = 0xfffe, .d = 0xffff, .x = 0xffff, .y = 0xffff},
                                            {.pc = 1, .d = 0, .x = 1, .y = 0}}};
  for (const auto operation : operations) {
    for (auto before : cases) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        before.ccr = static_cast<Byte>(ccr);
        one(core, {.operation = operation, .before = before});
      }
    }
  }
}
}

void divisions(Core &core) {
  sixteen_bit(core, Operation::idiv);
  sixteen_bit(core, Operation::idivs);
  sixteen_bit(core, Operation::fdiv);
  for (const auto operation : {Operation::ediv, Operation::edivs}) {
    lower_domain(core, operation);
    upper_domain(core, operation);
    divisor_domain(core, operation);
  }
  directed_flags(core);
}
}
