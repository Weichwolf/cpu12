// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "jit/emission.hpp"

#include <array>
#include <bit>
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
  bool joined_ = false;
  bool flags_live_ = true;
  std::vector<ExitFixup> exits_;
  static constexpr unsigned byte_bits = 8;
  static constexpr unsigned byte_mask = 0xff;
  static constexpr auto pc_offset = static_cast<std::uint8_t>(offsetof(Registers, pc));
  static constexpr std::array<std::uint8_t, 4> pc_add{0x66, 0x81, 0x47, pc_offset};

public:
  explicit Code(const bool joined = false, const bool flags_live = true) noexcept
      : joined_(joined), flags_live_(flags_live) {}

  [[nodiscard]] bool flags_live() const noexcept { return flags_live_; }

  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

  [[nodiscard]] Emission emission() && {
    return {.bytes = std::move(bytes_), .exits = std::move(exits_)};
  }

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

  // Preserve staged values across real external calls when an instruction needs them.
  // Sixteen bytes preserve SysV alignment; guest stack and CPU remain separate.
  void frame() {
    if (framed_) { throw std::logic_error("Native frame already exists"); }
    constexpr std::array<std::uint8_t, 4> reserve{0x48, 0x83, 0xec, 0x10};
    raw(reserve);
    framed_ = true;
  }

  [[nodiscard]] std::uint8_t return_size() const {
    return static_cast<std::uint8_t>((framed_ ? 4 : 0) + (joined_ ? 5 : 1));
  }

  void return_(const NativeExit kind = NativeExit::complete) {
    if (framed_) {
      constexpr std::array<std::uint8_t, 4> release{0x48, 0x83, 0xc4, 0x10};
      raw(release);
    }
    if (joined_) {
      constexpr std::uint8_t jump = 0xe9;
      byte(jump);
      exits_.push_back({.displacement = relative32(), .kind = kind});
    } else {
      constexpr std::uint8_t ret = 0xc3;
      byte(ret);
    }
  }

  // Compile-time fixups only; no branch table or dispatcher in generated code.
  [[nodiscard]] std::size_t relative32() {
    const auto position = bytes_.size();
    u32(0);
    return position;
  }

  void bind(const std::size_t position) { bind(position, bytes_.size()); }

  void bind(const std::size_t position, const std::size_t target) {
    constexpr std::size_t displacement_bytes = sizeof(std::uint32_t);
    if (position > bytes_.size() || bytes_.size() - position < displacement_bytes ||
        target > bytes_.size()) {
      throw std::logic_error("Invalid native branch fixup");
    }
    const auto origin = position + displacement_bytes;
    const auto distance = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(origin);
    if (distance < std::numeric_limits<std::int32_t>::min() ||
        distance > std::numeric_limits<std::int32_t>::max()) {
      throw std::length_error("Native branch displacement exceeds rel32");
    }
    const auto value = std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(distance));
    for (unsigned index = 0; index < displacement_bytes; ++index) {
      bytes_[position + index] = static_cast<std::uint8_t>(value >> (index * byte_bits));
    }
  }
};

}
