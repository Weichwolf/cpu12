// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cpu12::detail {
inline constexpr unsigned flag_c = 1, flag_v = 2, flag_z = 4, flag_n = 8, flag_h = 0x20;
inline constexpr unsigned flags_nzv = flag_n | flag_z | flag_v;
inline constexpr unsigned flags_nzvc = flags_nzv | flag_c;

struct ResidentOperation {
  Instruction instruction;
  Address pc = 0;
  unsigned flags = 0;
  unsigned forwarded_word = 0, forwarded_shift = 0;
  std::uint8_t value_slot = 0;
  bool fill_value = false;
  bool deferred = false, branch_zero = false, initial_load = false;
  bool cached_operand = false, cache_store = false;
  const Byte *pointer = nullptr;
  std::int32_t displacement = 0;
};

struct ResidentPlan {
  std::vector<ResidentOperation> operations;
  const Byte *base = nullptr;
  Ram indexed_region;
  unsigned changed = 0, used_words = 0, dirty_words = 0;
  unsigned counter_offset = 0;
  // A separate guest backedge retires on every iteration except the final exit.
  unsigned backedge_instructions = 0;
  ExitLocation backedge_exit{};
  bool swapped_d = false, loop = false, increment = false, host_zero = false;
};

[[nodiscard]] std::optional<ResidentPlan>
resident_plan(std::span<const BlockInstruction> instructions, const Memory &memory);
[[nodiscard]] std::optional<ResidentPlan>
resident_loop_plan(std::span<const BlockInstruction> instructions, const Memory &memory);
[[nodiscard]] unsigned written_flags(const Instruction &instruction);
[[nodiscard]] unsigned written_words(const Instruction &instruction);
[[nodiscard]] unsigned native_word(const Instruction &instruction);
[[nodiscard]] std::vector<std::uint8_t> emit_resident(const ResidentPlan &plan,
                                                      std::span<const std::uint8_t> general);
}
