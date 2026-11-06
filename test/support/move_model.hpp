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
#include <vector>

namespace cpu12::test::move {
using Field = std::uint16_t Registers::*;
constexpr unsigned byte_bits = 8, byte_mask = 0xff, word_mask = 0xffff;
constexpr Registers seed{
    .pc = 0xfffe, .d = 0x80ff, .x = 0x4000, .y = 0x4100, .sp = 0xffff, .ccr = 0xff};
inline std::uint64_t checked = 0;
enum class Offset : Byte { none, a, b, d };

struct Index {
  Field base = &Registers::x;
  int constant = 0, update = 0;
  Offset accumulator = Offset::none;
  bool pre = false;
  Byte postbyte = 0;
};

struct Base {
  Field field;
  Byte small, other;
};

inline void auto_indexes(std::vector<Index> &result, const Base base) {
  if (base.field == &Registers::pc) { return; }
  for (int delta = -8; delta <= 8; ++delta) {
    if (delta == 0) { continue; }
    for (const bool pre : {false, true}) {
      constexpr unsigned auto_mode = 0x20;
      constexpr unsigned post = 0x10;
      const auto raw =
          delta > 0 ? static_cast<unsigned>(delta - 1) : 16U - static_cast<unsigned>(-delta);
      result.push_back(
          {.base = base.field,
           .update = delta,
           .pre = pre,
           .postbyte = static_cast<Byte>(base.small | auto_mode | (pre ? 0U : post) | raw)});
    }
  }
}

inline std::vector<Index> indexes() {
  constexpr std::array bases{Base{.field = &Registers::x, .small = 0, .other = 0xe0},
                             Base{.field = &Registers::y, .small = 0x40, .other = 0xe8},
                             Base{.field = &Registers::sp, .small = 0x80, .other = 0xf0},
                             Base{.field = &Registers::pc, .small = 0xc0, .other = 0xf8}};
  std::vector<Index> result;
  for (const auto base : bases) {
    for (int offset = -16; offset <= 15; ++offset) {
      constexpr unsigned mask = 31;
      result.push_back(
          {.base = base.field,
           .constant = offset,
           .postbyte = static_cast<Byte>(base.small | (static_cast<unsigned>(offset) & mask))});
    }
    auto_indexes(result, base);
    constexpr std::array offsets{Offset::a, Offset::b, Offset::d};
    for (unsigned i = 0; i < offsets.size(); ++i) {
      result.push_back({.base = base.field,
                        .accumulator = offsets[i],
                        .postbyte = static_cast<Byte>(base.other + 4 + i)});
    }
  }
  return result;
}

enum class Mode : Byte { immediate, extended, indexed };

struct Operand {
  Mode mode = Mode::immediate;
  std::uint16_t address = 0;
  Index index{};
};

struct Point {
  bool word;
  Operand source, destination;
  Registers before;
  unsigned data;
};

struct Code {
  std::array<Byte, maximum_capture_bytes> bytes{};
  std::size_t length = 0;

  void append(const Byte data) { bytes.at(length++) = data; }

  void word(const unsigned data) {
    append(static_cast<Byte>(data >> byte_bits));
    append(static_cast<Byte>(data));
  }

  [[nodiscard]] std::span<const Byte> view() const { return std::span(bytes).first(length); }
};

inline Code encode(const Point point) {
  Code code;
  constexpr Byte prefix = 0x18;
  constexpr unsigned byte_page = 8;
  const auto form = static_cast<unsigned>(point.source.mode) +
                    (point.destination.mode == Mode::extended ? 3U : 0U);
  code.append(prefix);
  code.append(static_cast<Byte>(form + (point.word ? 0U : byte_page)));
  if (point.destination.mode == Mode::indexed) { code.append(point.destination.index.postbyte); }
  if (point.source.mode == Mode::indexed) {
    // IDX-IDX encodes source first, unlike IMM/EXT-IDX's destination-first layout.
    if (point.destination.mode == Mode::indexed) {
      code.bytes[2] = point.source.index.postbyte;
      code.append(point.destination.index.postbyte);
    } else {
      code.append(point.source.index.postbyte);
    }
  } else if (point.source.mode == Mode::extended) {
    code.word(point.source.address);
  } else if (point.word) {
    code.word(point.data);
  } else {
    code.append(static_cast<Byte>(point.data));
  }
  if (point.destination.mode == Mode::extended) { code.word(point.destination.address); }
  return code;
}

inline std::uint16_t resolve(const Operand operand, Registers &state, const std::size_t length) {
  if (operand.mode != Mode::indexed) { return operand.address; }
  const auto index = operand.index;
  int offset = index.constant + (index.pre ? index.update : 0);
  switch (index.accumulator) {
    case Offset::none: break;
    case Offset::a: offset += state.d >> byte_bits; break;
    case Offset::b: offset += static_cast<int>(state.d & byte_mask); break;
    case Offset::d: offset += state.d; break;
    default: throw std::runtime_error("Unknown independent MOV accumulator offset");
  }
  const auto original = state.*index.base;
  const auto next = index.base == &Registers::pc ? length : 0;
  const auto address =
      static_cast<std::uint16_t>(static_cast<int>(original) + static_cast<int>(next) + offset);
  state.*index.base = static_cast<std::uint16_t>(static_cast<int>(original) + index.update);
  return address;
}

struct Plan {
  Code code;
  Registers wanted;
  std::uint16_t source, destination;
};

inline Plan plan(const Point point) {
  const auto code = encode(point);
  auto wanted = point.before;
  const auto source = resolve(point.source, wanted, code.length);
  const auto destination = resolve(point.destination, wanted, code.length);
  wanted.pc = static_cast<std::uint16_t>(point.before.pc + code.length);
  return {.code = code, .wanted = wanted, .source = source, .destination = destination};
}

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline void retired(ObservedCore &core, const Point point, const Plan &wanted) {
  core.set_registers(point.before);
  const auto result = core.run_instruction(wanted.code.view());
  require(result.retired == 1 && result.fault == Fault::none && result.pc == point.before.pc &&
              result.opcode == wanted.code.bytes[0] && core.registers() == wanted.wanted,
          "MOV must match independent sequential source/destination addresses, index changes and "
          "unchanged CCR");
  ++checked;
}

void external_cases();
}
