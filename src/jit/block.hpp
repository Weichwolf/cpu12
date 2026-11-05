// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cpu12::detail {
struct BlockInstruction {
  Instruction instruction;
  Address pc = 0;
};

struct ExitLocation {
  Address pc = 0;
  Byte opcode = 0;
};

struct NativeResult {
  std::uint32_t retired = 0;
  ExitLocation location;
};

// The raw SysV result uses RAX for progress and RDX for the compact location.
struct RawNativeResult {
  std::uint64_t retired, location;
};

inline constexpr unsigned native_opcode_shift = 16;
// Fault exits and sleep instructions request a boundary without warm context rereads.
inline constexpr std::uint32_t native_boundary_mask = 1U << 24U;
class Code;
void describe_exit(Code &code, ExitLocation location, bool boundary = false);

inline constexpr unsigned native_page_shift = 8;
inline constexpr std::size_t native_page_entries = 1U << native_page_shift;
using NativePage = std::array<const void *, native_page_entries>;
using NativeIndex = std::array<const void *const *, native_page_entries>;

[[nodiscard]] constexpr NativeResult native_result(const RawNativeResult result) {
  return {.retired = static_cast<std::uint32_t>(result.retired),
          .location = {.pc = static_cast<Address>(result.location),
                       .opcode = static_cast<Byte>(result.location >> native_opcode_shift)}};
}

[[nodiscard]] std::vector<BlockInstruction> parse_block(const Memory &memory, Address start);
[[nodiscard]] std::vector<std::uint8_t> emit_block(std::span<const BlockInstruction> instructions,
                                                   const Memory &memory);
[[nodiscard]] NativeResult enter_block(const void *entry,
                                       Registers &registers,
                                       NativeContext &context,
                                       std::uint32_t budget) noexcept;
[[nodiscard]] NativeResult enter_blocks(const void *entry,
                                        Registers &registers,
                                        NativeContext &context,
                                        std::uint32_t budget,
                                        const NativeIndex &index) noexcept;

}
