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

// Ordinary borrowed storage. Empty known means initialized; otherwise zero is unknown.
// Read-only writes and unknown reads use the real I/O owner.
struct Ram {
  Address first = 0;
  std::span<Byte> bytes;
  std::span<Byte> known{static_cast<Byte *>(nullptr), 0};
  bool writable = true;
  std::uint64_t identity = 0;
};

// Borrowed, typed memory layout. No allocation or virtual memory dispatch.
class Memory {
public:
  [[nodiscard]] static constexpr Memory external() noexcept { return Memory(Ram{}); }

  [[nodiscard]] static Memory unified(const std::span<Byte> bytes) {
    if (bytes.empty()) { throw std::invalid_argument("Unified CPU12 memory must be nonempty"); }
    return window({.bytes = bytes});
  }

  [[nodiscard]] static Memory mapped(const std::span<const Ram> windows) {
    if (windows.empty()) {
      throw std::invalid_argument("Mapped CPU12 memory requires descriptors");
    }
    for (std::size_t index = 0; index < windows.size(); ++index) {
      validate(windows[index]);
      if (windows[index].bytes.empty()) { continue; }
      const auto first = windows[index].first;
      const auto end = first + windows[index].bytes.size();
      for (std::size_t earlier = 0; earlier < index; ++earlier) {
        if (windows[earlier].bytes.empty()) { continue; }
        const auto prior_first = windows[earlier].first;
        const auto prior_end = prior_first + windows[earlier].bytes.size();
        if (first < prior_end && prior_first < end) {
          throw std::invalid_argument("CPU12 memory grants overlap");
        }
      }
    }
    return Memory(windows);
  }

  [[nodiscard]] static Memory window(const Ram ram) {
    validate(ram);
    return Memory(ram);
  }

  [[nodiscard]] std::span<const Ram> regions() const noexcept {
    if (!windows_.empty()) { return windows_; }
    return ram_.bytes.empty() ? std::span<const Ram>{} : std::span<const Ram>{&ram_, 1};
  }

  [[nodiscard]] constexpr Ram storage() const noexcept { return ram_; }

private:
  static void validate(const Ram ram) {
    if (ram.bytes.size() > address_bytes - ram.first) {
      throw std::invalid_argument("RAM window exceeds the 16-bit CPU12 address space");
    }
    if (!ram.known.empty() && ram.known.size() != ram.bytes.size()) {
      throw std::invalid_argument("CPU12 knownness must match its byte grant");
    }
  }

  explicit constexpr Memory(const Ram ram) noexcept : ram_(ram) {}

  explicit constexpr Memory(const std::span<const Ram> windows) noexcept : windows_(windows) {}

  Ram ram_{};
  std::span<const Ram> windows_;
};
}
