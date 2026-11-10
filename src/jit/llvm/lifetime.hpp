// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <memory>
#include <type_traits>

namespace cpu12::detail::llvm19 {
template <auto Dispose> struct Disposer {
  void operator()(auto *value) const noexcept { Dispose(value); }
};

template <class Ref, auto Dispose>
using Owner = std::unique_ptr<std::remove_pointer_t<Ref>, Disposer<Dispose>>;

}
