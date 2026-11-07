// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"

#include <cstdint>

namespace cpu12::test::fft {
struct HostResult {
  std::uint32_t d;
  std::uintptr_t borrow;
};

struct HostMemory {
  Byte *u, *v, *scratch;
  const Byte *coefficient;
};
}
