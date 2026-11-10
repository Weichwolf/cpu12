// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fft_host_kernel.hpp"
#include "fft_program.hpp"
#if defined(__aarch64__)
#include "fft_host_a64.hpp"
#else
#include "fft_host_x64.hpp"
#endif

#include <cstddef>
#include <utility>

namespace cpu12::test::fft {
template <auto Butterfly = host_butterfly, std::size_t... Indices>
inline HostResult host_butterflies(Image &ram, std::index_sequence<Indices...> schedule) {
  static_assert(decltype(schedule)::size() == butterfly_count);
  HostResult result{};
  // The authored guest unrolls the same fixed schedule. Resolve host addresses
  // statically too; keep every original product, temporary and output store.
  ((result = Butterfly({.u = ram.data() + data + (std::size_t{operations[Indices].u} * 4),
                        .v = ram.data() + data + (std::size_t{operations[Indices].v} * 4),
                        .scratch = ram.data() + product,
                        .coefficient = ram.data() + twiddles +
                                       (std::size_t{operations[Indices].coefficient} * 4)})),
   ...);
  return result;
}

template <auto Butterfly = host_butterfly>
inline Registers host_transform(Image &ram, Registers state) {
  const auto result = host_butterflies<Butterfly>(ram, std::make_index_sequence<butterfly_count>{});
  state.d = static_cast<Address>(result.d);
  state.y = word(ram, product);
  state.ccr = static_cast<Byte>((state.ccr & transform_retained_flags) | result.borrow |
                                (state.d == 0 ? 4U : 0U) | (state.d >= sign ? 8U : 0U));
  return state;
}

template <auto Butterfly = host_butterfly> inline Registers host(Image &ram, const Byte ccr) {
  auto state = host_transform<Butterfly>(ram, initial(static_cast<Byte>(ccr & retained_flags)));
  state.pc = return_pc;
  state.sp = stack - frame_bytes;
  return state;
}
}
