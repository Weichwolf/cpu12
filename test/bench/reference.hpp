// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace cpu12::bench {
inline constexpr unsigned biquad_coefficient_count = 20;
void fft(std::span<std::int16_t> data, std::span<const std::int16_t> coefficients);
[[nodiscard]] std::uint16_t control(unsigned transitions, std::uint16_t accumulator);
[[nodiscard]] std::uint16_t copy_checksum(std::span<const std::uint16_t> source,
                                          std::span<std::uint16_t> destination,
                                          unsigned repetitions);
[[nodiscard]] std::int16_t quantize(std::int64_t value);
void fir(std::span<const std::int16_t> input,
         std::span<const std::int16_t, 64> coefficients,
         std::span<std::int16_t> output);
[[nodiscard]] std::array<std::int16_t, 8>
biquad(std::span<const std::int16_t> input,
       std::span<const std::int16_t, biquad_coefficient_count> coefficients,
       std::array<std::int16_t, 8> state,
       std::span<std::int16_t> output);
}
