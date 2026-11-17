// SPDX-License-Identifier: GPL-3.0-only
#include "reference.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cpu12::bench {
std::int16_t quantize(const std::int64_t value) {
  constexpr std::int64_t scale = 32768;
  // Independently round the magnitude, unlike the guest's carry/bias sequence.
  const auto magnitude = value < 0 ? -value : value;
  auto whole = magnitude / scale;
  const auto remainder = magnitude % scale;
  if (remainder > scale / 2 || (remainder == scale / 2 && whole % 2 != 0)) { ++whole; }
  return static_cast<std::int16_t>(std::clamp(value < 0 ? -whole : whole, -scale, scale - 1));
}

void fir(const std::span<const std::int16_t> input,
         const std::span<const std::int16_t, 64> coefficients,
         const std::span<std::int16_t> output) {
  for (std::size_t sample = 0; sample < output.size(); ++sample) {
    std::int64_t sum = 0;
    for (std::size_t tap = 0; tap < coefficients.size(); ++tap) {
      sum += std::int64_t{input[sample + coefficients.size() - 1 - tap]} * coefficients[tap];
    }
    output[sample] = quantize(sum);
  }
}

std::array<std::int16_t, 8>
biquad(const std::span<const std::int16_t> input,
       const std::span<const std::int16_t, biquad_coefficient_count> coefficients,
       std::array<std::int16_t, 8> state,
       const std::span<std::int16_t> output) {
  constexpr std::int64_t scale = 32768;
  for (std::size_t sample = 0; sample < input.size(); ++sample) {
    auto value = input[sample];
    for (std::size_t stage = 0; stage < state.size() / 2; ++stage) {
      const auto c = coefficients.subspan(stage * 5, 5);
      const auto s = std::span(state).subspan(stage * 2, 2);
      const auto next = quantize((std::int64_t{s[0]} * scale) + (std::int64_t{value} * c[0]));
      const auto first = quantize((std::int64_t{s[1]} * scale) + (std::int64_t{value} * c[1]) -
                                  (std::int64_t{next} * c[3]));
      const auto second = quantize((std::int64_t{value} * c[2]) - (std::int64_t{next} * c[4]));
      s[0] = first;
      s[1] = second;
      value = next;
    }
    output[sample] = value;
  }
  return state;
}

std::uint16_t control(const unsigned transitions, std::uint16_t accumulator) {
  unsigned state = 3;
  for (unsigned step = 0; step < transitions; ++step) {
    accumulator = static_cast<std::uint16_t>((accumulator + state) ^ (state + 1));
    accumulator = static_cast<std::uint16_t>(accumulator + 3);
    state = (state + 3) % 8;
  }
  return accumulator;
}

std::uint16_t copy_checksum(const std::span<const std::uint16_t> source,
                            const std::span<std::uint16_t> destination,
                            const unsigned repetitions) {
  std::uint16_t checksum = 0;
  for (unsigned pass = 0; pass < repetitions; ++pass) {
    for (std::size_t word = 0; word < source.size(); ++word) {
      destination[word] = source[word];
      checksum = static_cast<std::uint16_t>(checksum + source[word]);
    }
  }
  return checksum;
}

void fft(const std::span<std::int16_t> data, const std::span<const std::int16_t> coefficients) {
  const auto points = data.size() / 2;
  const auto halve = [](const int value) {
    return static_cast<std::int16_t>(value >= 0 ? value / 2 : -((-value + 1) / 2));
  };
  for (std::size_t width = 2; width <= points; width *= 2) {
    for (std::size_t group = 0; group < points; group += width) {
      for (std::size_t lane = 0; lane < width / 2; ++lane) {
        const auto u = (group + lane) * 2;
        const auto v = u + width;
        const auto coefficient = ((lane * points) / width) * 2;
        const auto real = quantize((std::int64_t{data[v]} * coefficients[coefficient]) -
                                   (std::int64_t{data[v + 1]} * coefficients[coefficient + 1]));
        const auto imag = quantize((std::int64_t{data[v]} * coefficients[coefficient + 1]) +
                                   (std::int64_t{data[v + 1]} * coefficients[coefficient]));
        const auto ur = data[u];
        const auto ui = data[u + 1];
        data[u] = halve(ur + real);
        data[u + 1] = halve(ui + imag);
        data[v] = halve(ur - real);
        data[v + 1] = halve(ui - imag);
      }
    }
  }
}
}
