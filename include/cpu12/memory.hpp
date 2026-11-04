// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace cpu12 {
using Byte = std::uint8_t;
using Address = std::uint16_t;
inline constexpr std::size_t address_bytes = 65536;

// A known writable byte window. The owner keeps its storage stable and alive.
struct Ram {
  Address first = 0;
  std::span<Byte> bytes;
};

// Borrowed, typed memory layout. No allocation or virtual memory dispatch.
class Memory {
public:
  [[nodiscard]] static constexpr Memory external() noexcept { return Memory({}); }

  [[nodiscard]] static Memory unified(const std::span<Byte> bytes) {
    if (bytes.empty()) { throw std::invalid_argument("Unified CPU12 memory must be nonempty"); }
    return window({.bytes = bytes});
  }

  [[nodiscard]] static Memory window(const Ram ram) {
    if (ram.bytes.size() > address_bytes - ram.first) {
      throw std::invalid_argument("RAM window exceeds the 16-bit CPU12 address space");
    }
    return Memory(ram);
  }

  [[nodiscard]] constexpr Ram storage() const noexcept { return ram_; }

private:
  explicit constexpr Memory(const Ram ram) noexcept : ram_(ram) {}

  Ram ram_;
};
}
