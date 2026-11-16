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
// Authored straight-line operations end in a real BRA loop. Every operation/PC
// combination is compiled before execution and installed against its actual ROM.
class InstructionCore : public Core {
public:
  InstructionCore(const std::span<const std::vector<Byte>> operations,
                  const std::span<const Address> positions,
                  Io *const io = nullptr)
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
      source.code[0] = {.address = pc, .words = source.bytes};
      const auto first = std::min(source.bytes.size(), address_bytes - pc);
      source.memory.push_back(
          {.first = pc, .bytes = std::span(source.bytes).first(first), .writable = false});
      if (first != source.bytes.size()) {
        source.memory.push_back(
            {.bytes = std::span(source.bytes).subspan(first), .writable = false});
      }
      for (const auto region : source.memory) {
        source.layout.push_back(
            {.first = region.first, .size = region.bytes.size(), .writable = false});
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
    std::array<CodeSpan, 1> code{};
    std::vector<Ram> memory;
    std::vector<RegionLayout> layout;
  };

  std::vector<Source> sources_;
  NativeImage image_;
  std::size_t selected_ = static_cast<std::size_t>(-1);
};
}
