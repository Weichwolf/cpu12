// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace cpu12::detail {
struct NativeContext;
}

extern "C" {
[[nodiscard]] std::uint32_t cpu12_read_byte(cpu12::detail::NativeContext *context,
                                            std::uint16_t address) noexcept;
[[nodiscard]] std::uint32_t cpu12_read_word(cpu12::detail::NativeContext *context,
                                            std::uint16_t address) noexcept;
[[nodiscard]] std::uint32_t cpu12_write_byte(cpu12::detail::NativeContext *context,
                                             std::uint16_t address,
                                             std::uint8_t value) noexcept;
[[nodiscard]] std::uint32_t cpu12_write_word(cpu12::detail::NativeContext *context,
                                             std::uint16_t address,
                                             std::uint16_t value) noexcept;
}
