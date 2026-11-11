// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SHA256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace cpu12::detail::llvm19 {
class Identity {
public:
  void add(const std::uint64_t value) {
    std::array<std::uint8_t, sizeof(value)> bytes{};
    constexpr unsigned byte_bits = 8;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<std::uint8_t>(value >> (i * byte_bits));
    }
    hash_.update(bytes);
  }

  void add(const std::span<const std::byte> bytes) {
    add(bytes.size());
    hash_.update(
        llvm::ArrayRef(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()));
  }

  void add(const std::string_view text) { add(std::as_bytes(std::span(text.data(), text.size()))); }

  [[nodiscard]] std::string finish() {
    constexpr std::string_view digits = "0123456789abcdef";
    constexpr unsigned half_byte = 4;
    constexpr unsigned mask = 15;
    std::string result;
    for (const auto byte : hash_.final()) {
      result += digits[byte >> half_byte];
      result += digits[byte & mask];
    }
    return result;
  }

private:
  llvm::SHA256 hash_;
};
}
