// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>

namespace cpu12::detail {
struct Capture {
  std::array<Byte, maximum_capture_bytes> bytes{};
  std::size_t size = 0;
  bool supported = false;
};

[[nodiscard]] Capture fetch(const Memory &ram, NativeContext &context, Address pc);
}
