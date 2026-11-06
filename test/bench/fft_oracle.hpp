// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fft_program.hpp"

#include <cstdint>

namespace cpu12::test::fft {
inline int signed_word(const Address value) {
  return value < sign ? int{value} : int{value} - int{word_mask + 1};
}

inline Address narrow(const std::int64_t value) {
  return static_cast<Address>(value);
}

inline Address product_value(Image &ram, const Address left, const Address right) {
  const auto full = std::int64_t{signed_word(left)} * signed_word(right);
  const auto bits = static_cast<std::uint32_t>(full);
  word(ram, product, static_cast<Address>(bits >> 16U));
  word(ram, product + 2, static_cast<Address>(bits & word_mask));
  constexpr std::int64_t scale = 256;
  // Arithmetic definition independent of the guest's middle-word extraction.
  return narrow(full >= 0 ? full / scale : -((-full + scale - 1) / scale));
}

inline Registers predict_transform(Image &ram, Registers state) {
  bool borrow = false;
  // Derive stage pairs directly; do not consume the guest assembly schedule.
  for (unsigned stage = 1; stage <= stages; ++stage) {
    const unsigned span = 1U << stage;
    for (unsigned index = 0; index < points; ++index) {
      if ((index & (span / 2)) != 0) { continue; }
      const auto u = static_cast<Address>(data + (index * 4));
      const auto v = static_cast<Address>(u + (span * 2));
      const auto coefficient =
          static_cast<Address>(twiddles + ((index % (span / 2)) * points / span * 4));
      const auto ur = word(ram, u);
      const auto ui = word(ram, u + 2);
      const auto vr = word(ram, v);
      const auto vi = word(ram, v + 2);
      const auto wr = word(ram, coefficient);
      const auto wi = word(ram, coefficient + 2);
      const auto first = product_value(ram, vr, wr);
      word(ram, real, first);
      const auto second = product_value(ram, vi, wi);
      word(ram, term, second);
      const auto tr = narrow(std::int64_t{first} - second);
      word(ram, real, tr);
      const auto third = product_value(ram, vr, wi);
      word(ram, imaginary, third);
      const auto fourth = product_value(ram, vi, wr);
      const auto ti = narrow(std::int64_t{third} + fourth);
      word(ram, imaginary, ti);
      word(ram, previous, ur);
      word(ram, u, narrow(std::int64_t{ur} + tr));
      word(ram, v, narrow(std::int64_t{ur} - tr));
      word(ram, previous, ui);
      word(ram, u + 2, narrow(std::int64_t{ui} + ti));
      word(ram, v + 2, narrow(std::int64_t{ui} - ti));
      borrow = ui < ti;
    }
  }
  state.d = word(ram, data + ((points - 1) * 4) + 2);
  state.y = word(ram, product);
  state.ccr = static_cast<Byte>((state.ccr & transform_retained_flags) | (borrow ? 1U : 0U) |
                                (state.d == 0 ? 4U : 0U) | (state.d >= sign ? 8U : 0U));
  return state;
}

inline Registers predict(Image &ram, const Byte ccr) {
  auto state = predict_transform(ram, initial(static_cast<Byte>(ccr & retained_flags)));
  state.pc = return_pc;
  state.sp = stack - frame_bytes;
  frame(ram, state);
  return state;
}
}
