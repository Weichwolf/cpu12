// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "move_model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cpu12::test::prepared_move {
using move::require;
constexpr Address first = 0x4000, input = 0x2000, output = 0x2100;
constexpr unsigned sample = 0xa55a;
constexpr unsigned byte_bits = 8, byte_mask = 0xff, word_mask = 0xffff;
constexpr std::uint64_t variant = 1;
constexpr std::array<unsigned, 8> edges{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xa55a, 0xffff};
constexpr Registers initial{
    .pc = first, .d = 0x80ff, .x = input, .y = output, .sp = 0, .ccr = byte_mask};
constexpr std::array<Byte, 2> idle{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
constexpr move::Operand from{.mode = move::Mode::extended, .address = input};
constexpr move::Operand to{.mode = move::Mode::extended, .address = output};
constexpr move::Operand x{.mode = move::Mode::indexed};
constexpr move::Operand y{.mode = move::Mode::indexed,
                          .index = {.base = &Registers::y, .postbyte = 0x40}};

enum class Group : std::uint8_t { ordinary, x, y, sp, external };

struct Site {
  move::Point point;
  move::Plan plan;
  bool full = false;

  [[nodiscard]] unsigned cycles() const {
    const auto &source = point.source;
    const auto &destination = point.destination;
    if (source.mode == move::Mode::immediate) {
      return point.word && destination.mode == move::Mode::extended ? 5 : 4;
    }
    return source.mode == move::Mode::extended && destination.mode == move::Mode::extended ? 6 : 5;
  }
};

struct Source {
  explicit Source(const Group group) {
    if (group == Group::ordinary || group == Group::external) {
      ordinary(group == Group::ordinary);
    }
    if (group == Group::ordinary) {
      individual();
    } else if (group == Group::external) {
      external();
    } else {
      automatic(group);
    }
    for (const auto &site : sites) {
      entries.push_back(site.point.before.pc);
      spans.push_back({.address = site.point.before.pc,
                       .words = std::span(bytes).subspan(site.point.before.pc - first,
                                                         site.plan.code.length + idle.size())});
    }
  }

  void individual() {
    for (const auto index : move::indexes()) {
      for (const bool word : {false, true}) {
        const move::Operand indexed{.mode = move::Mode::indexed, .index = index};
        append({.word = word,
                .source = indexed,
                .destination = to,
                .before = initial,
                .data = sample});
        for (const auto source : {move::Operand{}, from}) {
          append({.word = word,
                  .source = source,
                  .destination = indexed,
                  .before = initial,
                  .data = sample});
        }
      }
    }
  }

  void pair(const move::Index source, const move::Index destination) {
    for (const bool word : {false, true}) {
      auto before = initial;
      before.*source.base = input;
      append({.word = word,
              .source = {.mode = move::Mode::indexed, .index = source},
              .destination = {.mode = move::Mode::indexed, .index = destination},
              .before = before,
              .data = sample});
    }
  }

  void automatic(const Group group) {
    auto field = &Registers::sp;
    if (group == Group::x) { field = &Registers::x; }
    if (group == Group::y) { field = &Registers::y; }
    const auto indexes = move::indexes();
    for (const auto source : indexes) {
      if (source.base != field || source.update == 0) { continue; }
      for (const auto destination : indexes) {
        if (destination.base == field && destination.update != 0) { pair(source, destination); }
      }
    }
  }

  void external() {
    const auto indexes = move::indexes();
    const auto chosen = [](const int update) { return update == -8 || update == 1 || update == 8; };
    for (const auto source : indexes) {
      if (!chosen(source.update)) { continue; }
      for (const auto destination : indexes) {
        if (source.base == destination.base && chosen(destination.update)) {
          pair(source, destination);
        }
      }
    }
  }

  void values(const move::Point point, const bool complete) {
    if (point.source.mode != move::Mode::immediate) {
      append(point, complete);
      return;
    }
    auto value = point;
    if (complete && !point.word) {
      for (unsigned data = 0; data <= byte_mask; ++data) {
        value.data = data;
        append(value);
      }
    } else {
      for (const auto data : edges) {
        value.data = point.word ? data : data & byte_mask;
        append(value);
      }
    }
  }

  void ordinary(const bool complete) {
    for (const bool word : {false, true}) {
      for (const auto source : {move::Operand{}, from, x}) {
        for (const auto destination : {to, y}) {
          values({.word = word,
                  .source = source,
                  .destination = destination,
                  .before = initial,
                  .data = 0},
                 complete);
        }
      }
    }
    const move::Operand last{.mode = move::Mode::extended, .address = word_mask};
    for (const bool word : {false, true}) {
      for (const auto source : {from, last}) {
        for (const auto destination : {source, last}) {
          append({.word = word,
                  .source = source,
                  .destination = destination,
                  .before = initial,
                  .data = sample});
        }
      }
    }
  }

  void append(move::Point point, const bool full = false) {
    constexpr unsigned stride = 8;
    point.before.pc = static_cast<Address>(first + bytes.size());
    if (!point.word) { point.data &= byte_mask; }
    const Site site{.point = point, .plan = move::plan(point), .full = full};
    sites.push_back(site);
    const auto offset = bytes.size();
    bytes.resize(offset + stride);
    auto destination = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    std::ranges::copy(site.plan.code.view(), destination);
    std::ranges::copy(idle, destination + static_cast<std::ptrdiff_t>(site.plan.code.length));
  }

  std::vector<Byte> bytes;
  std::vector<Site> sites;
  std::vector<Address> entries;
  std::vector<CodeSpan> spans;
};

inline NativeImage compile(const Source &source, const bool external) {
  const auto last = first + source.bytes.size();
  const std::array regions{
      RegionLayout{.first = first, .size = source.bytes.size(), .writable = false},
      RegionLayout{.first = 0, .size = first},
      RegionLayout{.first = static_cast<Address>(last), .size = address_bytes - last}};
  const std::array layouts{VariantLayout{
      .id = variant, .memory = std::span(regions).first(external ? 1 : regions.size())}};
  const std::array variants{
      CodeVariant{.id = variant, .code = source.spans, .entries = source.entries}};
  return Core::compile({.variants = variants}, {.memory = layouts, .interrupts = {}});
}

void bus_cases();
}
