// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <cstdint>

namespace cpu12::detail {
inline constexpr std::uint32_t failed_read = 0x100;

struct NativeContext {
  Io io;
  Fault fault = Fault::none;
  std::uint16_t address = 0;
  std::uint8_t value = 0;
  std::uint64_t read_callbacks = 0, write_callbacks = 0;
};

[[nodiscard]] std::uint32_t read_byte(NativeContext *context, std::uint16_t address) noexcept;
[[nodiscard]] std::uint32_t
write_byte(NativeContext *context, std::uint16_t address, std::uint8_t value) noexcept;
}
