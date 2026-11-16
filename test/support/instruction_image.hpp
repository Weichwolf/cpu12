// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::test {
// Authored operations end in a real BRA loop, with explicit extra landings/data.
// Every operation/PC is prepared up front and installed against its actual ROM.
class InstructionCore : public Core {
public:
  InstructionCore(const std::span<const std::vector<Byte>> operations,
                  const std::span<const Address> positions,
                  Io *const io = nullptr,
                  const std::span<const Address> landings = {},
                  const std::span<const Ram> data = {})
      : Core(Memory::external(), io) {
    sources_.reserve(operations.size() * positions.size());
    for (const auto pc : positions) {
      for (const auto &operation : operations) {
        auto &source = sources_.emplace_back();
        source.entry[0] = pc;
        source.bytes = operation;
        source.bytes.push_back(static_cast<Byte>(isa::Opcode::bra_relative));
        constexpr Byte self = 0xfe;
        source.bytes.push_back(self);
      }
    }
    std::vector<CodeVariant> variants;
    std::vector<VariantLayout> layouts;
    for (auto &source : sources_) {
      const auto id = variants.size();
      const auto pc = source.entry[0];
      source.code.push_back({.address = pc, .words = source.bytes});
      for (const auto landing : landings) {
        source.code.push_back({.address = landing, .words = source.terminal});
      }
      for (const auto span : source.code) {
        const auto first = std::min(span.words.size(), address_bytes - span.address);
        // CodeSpan is read-only; the matching backing vectors remain owned here.
        const auto bytes =
            span.address == pc ? std::span(source.bytes) : std::span(source.terminal);
        source.memory.push_back(
            {.first = span.address, .bytes = bytes.first(first), .writable = false});
        if (first != bytes.size()) {
          source.memory.push_back({.bytes = bytes.subspan(first), .writable = false});
        }
      }
      source.memory.insert(source.memory.end(), data.begin(), data.end());
      for (const auto region : source.memory) {
        source.layout.push_back({.first = region.first,
                                 .size = region.bytes.size(),
                                 .writable = region.writable,
                                 .tracked = !region.known.empty()});
      }
      variants.push_back({.id = id, .code = source.code, .entries = source.entry});
      layouts.push_back({.id = id, .memory = source.layout});
    }
    image_ = Core::compile(
        {.variants = variants},
        {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
  }

  void select(const std::span<const Byte> operation, const Address pc) {
    const auto matches = [&](const Source &source) {
      return source.entry[0] == pc && source.bytes.size() == operation.size() + 2 &&
             std::ranges::equal(std::span(source.bytes).first(operation.size()), operation);
    };
    if (selected_ < sources_.size() && matches(sources_[selected_])) { return; }
    const auto found = std::ranges::find_if(sources_, matches);
    if (found == sources_.end()) {
      throw std::invalid_argument("ISA test selected an undeclared operation/PC");
    }
    selected_ = static_cast<std::size_t>(found - sources_.begin());
    set_memory(Memory::mapped(found->memory));
    install(image_, selected_);
  }

private:
  struct Source {
    std::array<Address, 1> entry{};
    std::vector<Byte> bytes;
    std::array<Byte, 2> terminal{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
    std::vector<CodeSpan> code;
    std::vector<Ram> memory;
    std::vector<RegionLayout> layout;
  };

  std::vector<Source> sources_;
  NativeImage image_;
  std::size_t selected_ = static_cast<std::size_t>(-1);
};
}
