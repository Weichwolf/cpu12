// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::waits {
using Image = std::array<Byte, address_bytes>;
constexpr Registers seed{.pc = 0xffff, .d = 0x1234, .x = 0x5678, .y = 0x9abc, .sp = 4, .ccr = 0x20};
constexpr Address vector = 0xfff2, handler = 0x400;
constexpr std::uint32_t idle_budget = 100;
constexpr std::array<Byte, 1> rti{0x0b}, nop{0xa7};

struct Form {
  std::array<Byte, 2> code;
  std::uint8_t length;
  ExecutionState state;

  [[nodiscard]] std::span<const Byte> bytes() const { return std::span(code).first(length); }
};

constexpr std::array<Form, 2> forms{
    {{.code = {0x3e}, .length = 1, .state = ExecutionState::waiting},
     {.code = {0x18, 0x3e}, .length = 2, .state = ExecutionState::stopped}}};
inline std::uint64_t entered = 0;

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline Registers sleeping(const Registers before, const Form form) {
  auto after = before;
  after.pc = static_cast<Address>(before.pc + form.length);
  after.sp = static_cast<Address>(before.sp + address_bytes - 9U);
  return after;
}

inline void check_frame(const Image &image, const Registers before, const Form form) {
  const auto after = sleeping(before, form);
  constexpr unsigned bits = 8;
  constexpr unsigned mask = 0xff;
  const std::array<Byte, 9> expected{before.ccr,
                                     static_cast<Byte>(before.d & mask),
                                     static_cast<Byte>(before.d >> bits),
                                     static_cast<Byte>(before.x >> bits),
                                     static_cast<Byte>(before.x & mask),
                                     static_cast<Byte>(before.y >> bits),
                                     static_cast<Byte>(before.y & mask),
                                     static_cast<Byte>(after.pc >> bits),
                                     static_cast<Byte>(after.pc & mask)};
  for (unsigned index = 0; index < expected.size(); ++index) {
    require(image[static_cast<Address>(after.sp + index)] == expected[index],
            "WAI/STOP frame preserves original CCR,B,A,X,Y and the actual following PC");
  }
}

inline void enter(ObservedCore &core, const Image &image, const Registers before, const Form form) {
  const auto result = core.run_instruction(form.bytes());
  require(result.retired == 1 && result.fault == Fault::none && result.state == form.state &&
              result.pc == before.pc && core.state().execution == form.state &&
              core.registers() == sleeping(before, form),
          "A native wait retires once, saves next PC, changes SP and preserves every other "
          "flag/register");
  check_frame(image, before, form);
  ++entered;
}

void bus_cases();
void state_cases();
}
