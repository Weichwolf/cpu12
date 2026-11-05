// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "runtime.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace cpu12::detail {
struct BlockInstruction {
  Instruction instruction;
  Address pc = 0;
};

struct NativeBlock {
  std::vector<BlockInstruction> instructions;
  const void *entry = nullptr;
};

[[nodiscard]] std::vector<BlockInstruction> parse_block(const Memory &memory, Address start);
[[nodiscard]] std::vector<std::uint8_t> emit_block(std::span<const BlockInstruction> instructions,
                                                   const Memory &memory);
[[nodiscard]] std::uint32_t enter_block(const void *entry,
                                        Registers &registers,
                                        NativeContext &context,
                                        std::uint32_t budget) noexcept;
}
