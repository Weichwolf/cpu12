// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::detail {
class Code {
  static constexpr unsigned byte_bits = 8;
  static constexpr unsigned byte_mask = 0xff;
  static constexpr auto pc_offset = static_cast<std::uint8_t>(offsetof(Registers, pc));
  static constexpr std::array<std::uint8_t, 4> pc_add{0x66, 0x81, 0x47, pc_offset};

public:
  std::vector<std::uint8_t> bytes;

  void raw(const std::span<const std::uint8_t> values) {
    bytes.insert(bytes.end(), values.begin(), values.end());
  }

  void byte(const std::uint8_t value) { bytes.push_back(value); }

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

  // Compile-time fixups only; no branch table or dispatcher in generated code.
  [[nodiscard]] std::size_t relative32() {
    const auto position = bytes.size();
    u32(0);
    return position;
  }

  void bind(const std::size_t position) {
    constexpr std::size_t displacement_bytes = sizeof(std::uint32_t);
    if (position > bytes.size() || bytes.size() - position < displacement_bytes) {
      throw std::logic_error("Invalid native branch fixup");
    }
    const auto distance = bytes.size() - position - displacement_bytes;
    if (distance > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
      throw std::length_error("Native branch displacement exceeds rel32");
    }
    const auto value = static_cast<std::uint32_t>(distance);
    for (unsigned index = 0; index < displacement_bytes; ++index) {
      bytes[position + index] = static_cast<std::uint8_t>(value >> (index * byte_bits));
    }
  }
};

}
