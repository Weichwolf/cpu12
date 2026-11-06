// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <cstdint>
#include <limits>

namespace cpu12::detail {
inline constexpr std::uint32_t failed_read = 0x100;
inline constexpr std::uint32_t failed_word_read = 0x10000;

struct NativeContext {
  State cpu{};
  Io io;
  Fault fault = Fault::none;
  std::uint16_t address = 0;
  std::uint16_t value = 0;
  std::uint8_t access_size = 0;
  std::uint8_t read_mask = std::numeric_limits<std::uint8_t>::max();
};

[[nodiscard]] std::uint32_t read_byte(NativeContext *context, std::uint16_t address) noexcept;
[[nodiscard]] std::uint32_t read_masked_byte(NativeContext *context,
                                             MaskedByteRead access) noexcept;
[[nodiscard]] std::uint32_t
write_byte(NativeContext *context, std::uint16_t address, std::uint8_t value) noexcept;
[[nodiscard]] std::uint32_t read_word(NativeContext *context, std::uint16_t address) noexcept;
[[nodiscard]] std::uint32_t
write_word(NativeContext *context, std::uint16_t address, std::uint16_t value) noexcept;
}
