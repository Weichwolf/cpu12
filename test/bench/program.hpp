// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpu12::bench {
using Op = isa::Opcode;
inline constexpr Address code_base = 0x8000;
inline constexpr unsigned byte_bits = 8, byte_mask = 0xff;

// Independent fixture encoding, with checked relative and absolute label fixups.
class Program {
public:
  void op(const Op opcode) {
    const auto value = static_cast<unsigned>(opcode);
    if (value > byte_mask) { bytes_.push_back(static_cast<Byte>(value >> byte_bits)); }
    bytes_.push_back(static_cast<Byte>(value));
  }

  void byte(const Op opcode, const unsigned operand) {
    op(opcode);
    bytes_.push_back(static_cast<Byte>(operand));
  }

  void word(const Op opcode, const unsigned operand) {
    op(opcode);
    bytes_.push_back(static_cast<Byte>(operand >> byte_bits));
    bytes_.push_back(static_cast<Byte>(operand));
  }

  void label(const std::string &name) {
    if (!labels.emplace(name, code_base + bytes_.size()).second) {
      throw std::logic_error("Duplicate kernel label: " + name);
    }
  }

  void branch(const Op opcode, const std::string &name) {
    op(opcode);
    references.push_back({.offset = bytes_.size(), .name = name, .relative = true});
    bytes_.push_back(0);
  }

  void long_branch(const Op opcode, const std::string &name) {
    constexpr unsigned branch_page = 0x1800;
    op(static_cast<Op>(branch_page | static_cast<unsigned>(opcode)));
    references.push_back({.offset = bytes_.size(), .name = name, .relative = true, .wide = true});
    bytes_.insert(bytes_.end(), 2, 0);
  }

  void call(const std::string &name) {
    op(Op::jsr_extended);
    references.push_back({.offset = bytes_.size(), .name = name, .relative = false});
    bytes_.insert(bytes_.end(), 2, 0);
  }

  void dbne(const unsigned selector, const std::string &name) {
    byte(Op::dbne_relative, selector);
    references.push_back({.offset = bytes_.size(), .name = name, .relative = true, .loop = true});
    bytes_.push_back(0);
  }

  void resolve() {
    for (const auto &reference : references) {
      const auto target = labels.at(reference.name);
      const auto offset = reference.offset;
      if (!reference.relative) {
        bytes_[offset] = static_cast<Byte>(target >> byte_bits);
        bytes_[offset + 1] = static_cast<Byte>(target);
        continue;
      }
      const auto delta = static_cast<std::int64_t>(target) -
                         static_cast<std::int64_t>(code_base + offset + (reference.wide ? 2 : 1));
      if (reference.wide) {
        const auto word = static_cast<std::uint16_t>(delta);
        bytes_[offset] = static_cast<Byte>(word >> byte_bits);
        bytes_[offset + 1] = static_cast<Byte>(word);
        continue;
      }
      constexpr int short_min = -128;
      constexpr int short_max = 127;
      constexpr int loop_min = -256;
      constexpr int loop_max = 255;
      if (delta < (reference.loop ? loop_min : short_min) ||
          delta > (reference.loop ? loop_max : short_max)) {
        throw std::logic_error("Kernel relative branch out of range: " + reference.name);
      }
      if (reference.loop && delta < 0) { bytes_[offset - 1] |= 0x10; }
      bytes_[offset] = static_cast<Byte>(delta);
    }
  }

  [[nodiscard]] Address at(const std::string &name) const {
    return static_cast<Address>(labels.at(name));
  }

  [[nodiscard]] std::span<const Byte> code() const noexcept { return bytes_; }

  void root(const std::string &name) { entries_.push_back(at(name)); }

  [[nodiscard]] std::span<const Address> entries() const noexcept { return entries_; }

private:
  std::vector<Byte> bytes_;
  std::vector<Address> entries_{code_base};

  struct Reference {
    std::size_t offset;
    std::string name;
    bool relative, loop = false, wide = false;
  };

  std::map<std::string, std::size_t> labels;
  std::vector<Reference> references;
};
}
