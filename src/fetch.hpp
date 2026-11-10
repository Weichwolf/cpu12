// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace cpu12::detail {
struct Capture {
  std::array<Byte, maximum_capture_bytes> bytes{};
  std::size_t size = 0;
  bool supported = false;
};

// Decode one complete instruction at the start of an immutable borrowed window.
[[nodiscard]] std::optional<Instruction> decode_first(std::span<const Byte> bytes);

struct CodeCapture {
  std::optional<Instruction> instruction;
  RunResult failure;
};

[[nodiscard]] CodeCapture fetch_immutable(const Memory &memory, Address pc);

[[nodiscard]] Capture fetch(const Memory &ram, NativeContext &context, Address pc);
}
