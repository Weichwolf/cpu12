// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"

#include <span>

namespace cpu12::detail {
[[nodiscard]] std::span<const Byte> immutable_bytes(const Memory &memory, Address address);
}
