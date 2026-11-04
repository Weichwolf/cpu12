// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpu12::detail {
class Code {
  std::vector<std::uint8_t> bytes_;
  bool framed_ = false;
  static constexpr unsigned byte_bits = 8;
  static constexpr unsigned byte_mask = 0xff;
  static constexpr auto pc_offset = static_cast<std::uint8_t>(offsetof(Registers, pc));
  static constexpr std::array<std::uint8_t, 4> pc_add{0x66, 0x81, 0x47, pc_offset};

public:
  [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(bytes_); }

  void raw(const std::span<const std::uint8_t> values) {
    bytes_.insert(bytes_.end(), values.begin(), values.end());
  }

  void byte(const std::uint8_t value) { bytes_.push_back(value); }

  void u16(const std::uint16_t value) {
    byte(static_cast<std::uint8_t>(value & byte_mask));
    byte(static_cast<std::uint8_t>(value >> byte_bits));
  }

  void u32(const std::uint32_t value) {
    for (unsigned shift = 0; shift < sizeof(value) * byte_bits; shift += byte_bits) {
      byte(static_cast<std::uint8_t>((value >> shift) & byte_mask));
    }
  }

  void u64(const std::uint64_t value) {
    for (unsigned shift = 0; shift < sizeof(value) * byte_bits; shift += byte_bits) {
      byte(static_cast<std::uint8_t>((value >> shift) & byte_mask));
    }
  }

  void advance(const int delta) {
    raw(pc_add);
    u16(static_cast<std::uint16_t>(delta));
  }

  // Only indirect calls need to retain a resolved pointer across a real bus call.
  // Sixteen bytes preserve SysV alignment; guest stack and CPU remain separate.
  void frame() {
    if (framed_) { throw std::logic_error("Native frame already exists"); }
    constexpr std::array<std::uint8_t, 4> reserve{0x48, 0x83, 0xec, 0x10};
    raw(reserve);
    framed_ = true;
  }

  [[nodiscard]] std::uint8_t return_size() const { return framed_ ? 5 : 1; }

  void return_() {
    if (framed_) {
      constexpr std::array<std::uint8_t, 4> release{0x48, 0x83, 0xc4, 0x10};
      raw(release);
    }
    constexpr std::uint8_t ret = 0xc3;
    byte(ret);
  }

  // Compile-time fixups only; no branch table or dispatcher in generated code.
  [[nodiscard]] std::size_t relative32() {
    const auto position = bytes_.size();
    u32(0);
    return position;
  }

  void bind(const std::size_t position) {
    constexpr std::size_t displacement_bytes = sizeof(std::uint32_t);
    if (position > bytes_.size() || bytes_.size() - position < displacement_bytes) {
      throw std::logic_error("Invalid native branch fixup");
    }
    const auto distance = bytes_.size() - position - displacement_bytes;
    if (distance > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
      throw std::length_error("Native branch displacement exceeds rel32");
    }
    const auto value = static_cast<std::uint32_t>(distance);
    for (unsigned index = 0; index < displacement_bytes; ++index) {
      bytes_[position + index] = static_cast<std::uint8_t>(value >> (index * byte_bits));
    }
  }
};

}
