// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fft_host_kernel.hpp"
#include "fft_program.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace cpu12::test::fft {
inline Address cpp_word(const Byte *const memory) {
  return static_cast<Address>((unsigned{memory[0]} << byte_bits) | memory[1]);
}

inline void cpp_word(Byte *const memory, const Address value) {
  memory[0] = static_cast<Byte>(value >> byte_bits);
  memory[1] = static_cast<Byte>(value & byte_mask);
}

inline std::int32_t cpp_product(Byte *const scratch, const Address left, const Address right) {
  const auto full =
      std::int32_t{std::bit_cast<std::int16_t>(left)} * std::bit_cast<std::int16_t>(right);
  const auto bits = static_cast<std::uint32_t>(full);
  constexpr unsigned word_bits = 16;
  cpp_word(scratch, static_cast<Address>(bits >> word_bits));
  cpp_word(scratch + 2, static_cast<Address>(bits));
  return std::bit_cast<std::int16_t>(static_cast<Address>(full >> byte_bits));
}

inline HostResult cpp_butterfly(const HostMemory &memory) {
  const auto ur = cpp_word(memory.u);
  const auto ui = cpp_word(memory.u + 2);
  const auto vr = cpp_word(memory.v);
  const auto vi = cpp_word(memory.v + 2);
  const auto wr = cpp_word(memory.coefficient);
  const auto wi = cpp_word(memory.coefficient + 2);
  const auto first = cpp_product(memory.scratch, vr, wr);
  cpp_word(memory.scratch + 4, static_cast<Address>(first));
  const auto second = cpp_product(memory.scratch, vi, wi);
  cpp_word(memory.scratch + 8, static_cast<Address>(second));
  const auto tr = static_cast<Address>(first - second);
  cpp_word(memory.scratch + 4, tr);
  const auto third = cpp_product(memory.scratch, vr, wi);
  cpp_word(memory.scratch + 6, static_cast<Address>(third));
  const auto fourth = cpp_product(memory.scratch, vi, wr);
  const auto ti = static_cast<Address>(third + fourth);
  cpp_word(memory.scratch + 6, ti);
  cpp_word(memory.scratch + 10, ur);
  cpp_word(memory.u, static_cast<Address>(unsigned{ur} + tr));
  cpp_word(memory.v, static_cast<Address>(unsigned{ur} - tr));
  cpp_word(memory.scratch + 10, ui);
  cpp_word(memory.u + 2, static_cast<Address>(unsigned{ui} + ti));
  const auto output = static_cast<Address>(unsigned{ui} - ti);
  cpp_word(memory.v + 2, output);
  return {.d = output, .borrow = ui < ti ? 1U : 0U};
}
}
