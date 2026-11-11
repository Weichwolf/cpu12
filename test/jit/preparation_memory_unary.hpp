// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "memory_unary_model.hpp"
#include "unary_model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::test::prepared_memory_unary {
constexpr Address entry = 0x4000, operand = 0x2000, pointer = 0xffff;
constexpr unsigned byte_mask = 0xff, byte_bits = 8;
constexpr Address initial_d = 0xa55a;
constexpr std::uint64_t variant_id = 1;
constexpr std::array<Byte, 2> idle{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
constexpr std::array<Byte, 8> edges{0, 1, 2, 0x7f, 0x80, 0x81, 0xfe, 0xff};
using Operation = memory_unary::Operation;
enum class Form : std::uint8_t { extended, plain, pre_x, post_y, pre_sp, indirect };
enum class Backing : std::uint8_t { ram, tracked, external };
constexpr std::array forms{
    Form::extended, Form::plain, Form::pre_x, Form::post_y, Form::pre_sp, Form::indirect};

inline void require(const bool accepted, const char *message) {
  if (!accepted) { throw std::runtime_error(message); }
}

struct Site {
  Operation operation;
  Form form;
  Address pc;
  std::array<Byte, 4> code{};
  std::size_t length = 0;
};

inline Site encode(const Operation operation, const Form form, const Address pc) {
  Site site{.operation = operation, .form = form, .pc = pc};
  if (form == Form::extended) {
    site.code = {
        operation.extended, static_cast<Byte>(operand >> byte_bits), static_cast<Byte>(operand), 0};
    site.length = 3;
  } else {
    constexpr std::array<Byte, 6> postbytes{0, 0, 0x20, 0x71, 0xaf, 0xe3};
    site.code = {operation.indexed, postbytes[static_cast<std::size_t>(form)], 0, 0};
    site.length = form == Form::indirect ? 4 : 2;
  }
  return site;
}

inline Registers seed(const Site site, const Byte ccr) {
  Registers state{
      .pc = site.pc, .d = initial_d, .x = operand, .y = operand, .sp = operand + 1, .ccr = ccr};
  if (site.form == Form::pre_x) { --state.x; }
  if (site.form == Form::indirect) { state.x = pointer; }
  return state;
}

inline Registers expected(Registers state, const Site site, const Byte input) {
  const auto result = unary::expected(
      {.function = site.operation.function, .data = input, .ccr = state.ccr, .word = false});
  state.ccr = static_cast<Byte>(result.flags);
  state.pc = static_cast<Address>(site.pc + site.length);
  if (site.form == Form::pre_x) { ++state.x; }
  if (site.form == Form::post_y) { state.y += 2; }
  if (site.form == Form::pre_sp) { --state.sp; }
  return state;
}

inline unsigned cycles(const Site site) {
  const bool clear = !site.operation.read;
  const bool test = !site.operation.write;
  if (site.form == Form::extended) { return clear || test ? 3 : 4; }
  if (site.form == Form::indirect) {
    if (clear) { return 4; }
    return test ? 6 : 5;
  }
  return clear ? 2 : 3;
}

struct Source {
  Source() {
    constexpr std::size_t stride = 8;
    for (const auto operation : memory_unary::operations) {
      for (const auto form : forms) {
        const auto offset = bytes.size();
        const auto site = encode(operation, form, static_cast<Address>(entry + offset));
        sites.push_back(site);
        bytes.resize(offset + stride);
        std::ranges::copy(std::span(site.code).first(site.length),
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(idle, bytes.begin() + static_cast<std::ptrdiff_t>(offset + site.length));
      }
    }
    for (const auto site : sites) {
      entries.push_back(site.pc);
      spans.push_back(
          {.address = site.pc,
           .words = std::span(bytes).subspan(site.pc - entry, site.length + idle.size())});
    }
  }

  std::vector<Byte> bytes;
  std::vector<Site> sites;
  std::vector<Address> entries;
  std::vector<CodeSpan> spans;
};

inline NativeImage compile(const Source &source, const Backing backing) {
  const bool external = backing == Backing::external;
  const bool tracked = backing == Backing::tracked;
  const std::array regions{
      RegionLayout{.first = entry, .size = source.bytes.size(), .writable = false},
      RegionLayout{.first = operand, .size = 1, .tracked = tracked},
      RegionLayout{.first = pointer, .size = 1, .tracked = tracked},
      RegionLayout{.first = 0, .size = 1, .tracked = tracked}};
  const auto memory = std::span(regions).first(external ? 1 : regions.size());
  const std::array layouts{VariantLayout{.id = variant_id, .memory = memory}};
  const std::array variants{
      CodeVariant{.id = variant_id, .code = source.spans, .entries = source.entries}};
  return Core::compile({.variants = variants},
                       {.memory = layouts, .interrupts = {}, .transitions = {}});
}

void bus_cases(const Source &source);
}
