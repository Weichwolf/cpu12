// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cpu12::detail {
enum class NativeExit : std::uint8_t { complete, fault };

struct ExitFixup {
  std::size_t displacement = 0;
  NativeExit kind = NativeExit::complete;
};

struct EmissionOptions {
  bool flags_live = true, advance_pc = true;
};

struct Emission {
  std::vector<std::uint8_t> bytes;
  std::vector<ExitFixup> exits;
};
}
