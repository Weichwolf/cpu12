// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bits_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cpu12::test::prepared_bits {
using bits::require;
constexpr Address first = 0x4000, operand = 0x2000, direct_operand = 0xff;
constexpr Address initial_d = 0xa55a;

struct Input {
  Byte data, ccr;
};

constexpr unsigned byte_mask = 0xff, byte_bits = 8;
constexpr std::uint64_t variant = 1;
constexpr std::array<Byte, 8> edges{0, 1, 0x7f, 0x80, 0xfe, 0xff, 0x55, 0xaa};
constexpr std::array<Byte, 2> idle{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
enum class Form : std::uint8_t { extended, direct, plain, pre_x, post_y, pre_sp, pc, d };
constexpr std::array forms{Form::extended,
                           Form::direct,
                           Form::plain,
                           Form::pre_x,
                           Form::post_y,
                           Form::pre_sp,
                           Form::pc,
                           Form::d};

struct Site {
  bits::Operation operation;
  Form form;
  Address pc;
  Byte mask;
  bits::Code code;

  [[nodiscard]] Address address() const { return form == Form::direct ? direct_operand : operand; }

  [[nodiscard]] Registers before(const Byte ccr) const {
    Registers state{
        .pc = pc, .d = initial_d, .x = operand, .y = operand, .sp = operand + 1, .ccr = ccr};
    if (form == Form::pre_x) { --state.x; }
    if (form == Form::d) { state.x = static_cast<Address>(operand - state.d); }
    return state;
  }

  [[nodiscard]] bits::Point point(const Input input) const {
    const auto original = before(input.ccr);
    auto updated = original;
    if (form == Form::pre_x) { ++updated.x; }
    if (form == Form::post_y) { updated.y += 2; }
    if (form == Form::pre_sp) { --updated.sp; }
    constexpr unsigned taken_offset = 12;
    return {.operation = operation,
            .before = original,
            .updated = updated,
            .address = address(),
            .data = input.data,
            .mask = mask,
            .displacement = static_cast<int>(taken_offset - code.length)};
  }

  [[nodiscard]] unsigned cycles() const {
    if (form == Form::extended) { return operation.branch ? 5 : 4; }
    if (form == Form::pc) { return 6; }
    return 4;
  }
};

inline Site
encode(const bits::Operation operation, const Form form, const Byte mask, const Address pc) {
  Site site{.operation = operation, .form = form, .pc = pc, .mask = mask, .code = {}};
  auto &code = site.code;
  if (form == Form::extended || form == Form::direct) {
    code.append(form == Form::direct ? operation.direct : operation.extended);
    if (form == Form::extended) { code.append(static_cast<Byte>(operand >> byte_bits)); }
    code.append(static_cast<Byte>(site.address()));
  } else {
    code.append(operation.indexed);
    constexpr std::array<Byte, 8> posts{0, 0, 0, 0x20, 0x71, 0xaf, 0xfa, 0xe6};
    code.append(posts[static_cast<std::size_t>(form)]);
    if (form == Form::pc) {
      const unsigned length = operation.branch ? 6 : 5;
      const auto delta = static_cast<Address>(operand - static_cast<Address>(pc + length));
      code.append(static_cast<Byte>(delta >> byte_bits));
      code.append(static_cast<Byte>(delta));
    }
  }
  code.append(mask);
  if (operation.branch) {
    constexpr unsigned taken_offset = 12;
    code.append(static_cast<Byte>(taken_offset - (code.length + 1)));
  }
  return site;
}

struct Source {
  explicit Source(const bool complete_masks) {
    for (const auto operation : bits::operations) {
      for (const auto form : forms) {
        if (complete_masks && form == Form::extended) {
          for (unsigned mask = 0; mask <= byte_mask; ++mask) {
            append(operation, form, static_cast<Byte>(mask));
          }
        } else {
          for (const auto mask : edges) { append(operation, form, mask); }
        }
      }
    }
    for (const auto &site : sites) {
      entries.push_back(site.pc);
      spans.push_back(
          {.address = site.pc,
           .words = std::span(bytes).subspan(site.pc - first, site.code.length + idle.size())});
      if (site.operation.branch) {
        spans.push_back({.address = static_cast<Address>(site.pc + 12),
                         .words = std::span(bytes).subspan(site.pc + 12 - first, idle.size())});
      }
    }
  }

  void append(const bits::Operation operation, const Form form, const Byte mask) {
    constexpr unsigned stride = 16;
    constexpr unsigned taken_offset = 12;
    const auto offset = bytes.size();
    const auto site = encode(operation, form, mask, static_cast<Address>(first + offset));
    sites.push_back(site);
    bytes.resize(offset + stride);
    auto destination = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
    std::ranges::copy(site.code.view(), destination);
    std::ranges::copy(idle, destination + static_cast<std::ptrdiff_t>(site.code.length));
    std::ranges::copy(idle, destination + taken_offset);
  }

  std::vector<Byte> bytes;
  std::vector<Site> sites;
  std::vector<Address> entries;
  std::vector<CodeSpan> spans;
};

inline NativeImage compile(const Source &source, const bool external) {
  const std::array regions{
      RegionLayout{.first = first, .size = source.bytes.size(), .writable = false},
      RegionLayout{.first = operand, .size = 1},
      RegionLayout{.first = direct_operand, .size = 1}};
  const std::array layouts{VariantLayout{
      .id = variant, .memory = std::span(regions).first(external ? 1 : regions.size())}};
  const std::array variants{
      CodeVariant{.id = variant, .code = source.spans, .entries = source.entries}};
  return Core::compile({.variants = variants},
                       {.memory = layouts, .interrupts = {}, .transitions = {}});
}

void bus_cases();
}
