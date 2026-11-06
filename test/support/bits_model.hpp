// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::bits {
constexpr unsigned byte_mask = 0xff, byte_bits = 8;
constexpr Registers seed{.pc = 0xfffe, .d = 0x80ff, .x = 0, .y = 0x7fff, .sp = 0xffff, .ccr = 0xff};
inline std::uint64_t checked = 0;

struct Operation {
  Byte indexed, direct, extended;
  bool clear = false, branch = false;
};

constexpr std::array<Operation, 4> operations{
    {{.indexed = 0x0c, .direct = 0x4c, .extended = 0x1c},
     {.indexed = 0x0d, .direct = 0x4d, .extended = 0x1d, .clear = true},
     {.indexed = 0x0e, .direct = 0x4e, .extended = 0x1e, .branch = true},
     {.indexed = 0x0f, .direct = 0x4f, .extended = 0x1f, .clear = true, .branch = true}}};

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

struct Code {
  std::array<Byte, maximum_capture_bytes> bytes{};
  std::size_t length = 0;

  [[nodiscard]] std::span<const Byte> view() const { return std::span(bytes).first(length); }

  void append(const Byte byte) { bytes.at(length++) = byte; }
};

struct Mask {
  unsigned data, mask;
};

inline unsigned modified(const Operation operation, const Mask value) {
  return operation.clear ? value.data & (byte_mask ^ value.mask) : value.data | value.mask;
}

struct Point {
  Operation operation;
  Registers before, updated;
  std::uint16_t address;
  unsigned data, mask;
  int displacement = -3;
};

inline Registers expected(const Point point, const std::size_t length) {
  auto wanted = point.updated;
  int delta = static_cast<int>(length);
  if (point.operation.branch) {
    const unsigned selected = point.data & point.mask;
    const bool taken = point.operation.clear ? selected == 0 : selected == point.mask;
    if (taken) { delta += point.displacement; }
  } else {
    const auto data = modified(point.operation, {.data = point.data, .mask = point.mask});
    constexpr unsigned retained = 0xf1;
    constexpr unsigned sign = 0x80;
    wanted.ccr = static_cast<Byte>((point.before.ccr & retained) | (data >= sign ? 8U : 0U) |
                                   (data == 0 ? 4U : 0U));
  }
  wanted.pc = static_cast<std::uint16_t>(static_cast<int>(point.before.pc) + delta);
  return wanted;
}

inline void tail(Code &code, const Point point) {
  code.append(static_cast<Byte>(point.mask));
  if (point.operation.branch) { code.append(static_cast<Byte>(point.displacement)); }
}

inline Code absolute(const Point point, const bool direct) {
  Code code;
  code.append(direct ? point.operation.direct : point.operation.extended);
  if (!direct) { code.append(static_cast<Byte>(point.address >> byte_bits)); }
  code.append(static_cast<Byte>(point.address));
  tail(code, point);
  return code;
}

inline void retired(ObservedCore &core, const Code &code, const Point point) {
  core.set_registers(point.before);
  const auto result = core.run_instruction(code.view());
  require(result.retired == 1 && result.fault == Fault::none && result.pc == point.before.pc &&
              result.opcode == code.bytes[0] && core.registers() == expected(point, code.length),
          "Bit operation must match independent data, mask, full registers/CCR and relative-PC "
          "expectation");
  ++checked;
}

void external_cases();
}
