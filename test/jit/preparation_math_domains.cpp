// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "multiply_divide_model.hpp"
#include "preparation_math.hpp"

#include <array>
#include <cstdint>

namespace cpu12::test::prepared_math {
namespace {
void byte_product(Core &core, math::Input point, Count &count) {
  for (unsigned data = 0; data <= word_mask; ++data) {
    point.before.d = static_cast<Address>(data);
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      point.before.ccr = static_cast<Byte>(ccr);
      check(core, point, count);
    }
  }
}

void word_product(Core &core, math::Input point, Count &count) {
  for (const auto anchor : anchors) {
    for (unsigned data = 0; data <= word_mask; ++data) {
      point.before.d = static_cast<Address>(data);
      point.before.y = anchor;
      for (const auto ccr : flags) {
        point.before.ccr = ccr;
        check(core, point, count);
      }
      point.before.d = anchor;
      point.before.y = static_cast<Address>(data);
      point.before.ccr = byte_mask;
      check(core, point, count);
    }
  }
}

void word_dividend(Core &core, math::Input point, Count &count) {
  constexpr Address unchanged_y = 0x5aa5;
  point.before.y = unchanged_y;
  for (const auto divisor : anchors) {
    point.before.x = divisor;
    for (unsigned data = 0; data <= word_mask; ++data) {
      point.before.d = static_cast<Address>(data);
      for (const auto ccr : flags) {
        point.before.ccr = ccr;
        check(core, point, count);
      }
    }
  }
}

struct Scene {
  Address high, divisor;
};

void lower_domain(Core &core, math::Input point, Count &count) {
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
    point.before.y = scene.high;
    point.before.x = scene.divisor;
    for (unsigned low = 0; low <= word_mask; ++low) {
      point.before.d = static_cast<Address>(low);
      for (const auto ccr : flags) {
        point.before.ccr = ccr;
        check(core, point, count);
      }
    }
  }
}

void upper_domain(Core &core, math::Input point, Count &count) {
  constexpr std::array<Address, 4> divisors{1, 0x7fff, 0x8000, 0xffff};
  for (unsigned high = 0; high <= word_mask; ++high) {
    point.before.y = static_cast<Address>(high);
    for (const auto low : words) {
      point.before.d = low;
      for (const auto divisor : divisors) {
        point.before.x = divisor;
        check(core, point, count);
      }
    }
  }
}

void divisor_domain(Core &core, math::Input point, Count &count) {
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
    point.before.x = static_cast<Address>(divisor);
    for (const auto dividend : dividends) {
      point.before.y = static_cast<Address>(dividend >> word_bits);
      point.before.d = static_cast<Address>(dividend);
      check(core, point, count);
    }
  }
}
}

void domains(Core &core, const math::Input point, Count &count) {
  switch (point.operation) {
    case Operation::mul: byte_product(core, point, count); return;
    case Operation::emul:
    case Operation::emuls: word_product(core, point, count); return;
    case Operation::idiv:
    case Operation::idivs:
    case Operation::fdiv: word_dividend(core, point, count); return;
    case Operation::ediv:
    case Operation::edivs:
      lower_domain(core, point, count);
      upper_domain(core, point, count);
      divisor_domain(core, point, count);
      return;
    default: require(false, "Unknown independent math domain");
  }
}
}
