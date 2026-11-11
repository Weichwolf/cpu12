// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "divide_model.hpp"
#include "multiply_divide_model.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>

namespace cpu12::test::math {
namespace {
constexpr std::array<unsigned, 14> anchors{
    0, 1, 2, 3, 7, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff};
constexpr std::array<unsigned, 4> flags{0, byte_mask, 0xa5, 0x5a};
constexpr std::array<Operation, 5> operations{
    Operation::idiv, Operation::idivs, Operation::ediv, Operation::edivs, Operation::fdiv};

void one(ObservedCore &core, const Input point) {
  retired(core, point, divide_model::expected(point));
}

void sixteen_bit(ObservedCore &core, const Operation operation) {
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

void lower_domain(ObservedCore &core, const Operation operation) {
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

void upper_domain(ObservedCore &core, const Operation operation) {
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

void divisor_domain(ObservedCore &core, const Operation operation) {
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

void directed_flags(ObservedCore &core) {
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

void divisions(ObservedCore &core) {
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
