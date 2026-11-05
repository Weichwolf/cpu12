// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::fetching {
inline std::uint64_t captures = 0, native = 0;

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

void bus_cases();
void program_cases();
}
