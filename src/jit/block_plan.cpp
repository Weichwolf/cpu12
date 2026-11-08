// SPDX-License-Identifier: GPL-3.0-only
#include "jit/block_plan.hpp"

#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/emission.hpp"
#include "jit/resident.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace cpu12::detail {
std::vector<BlockInstruction> resident_terminal(const Memory &memory, const Address address) {
  auto result = parse_block(memory, address);
  if (result.size() == 1 && (result.front().instruction.kind == Kind::wait ||
                             result.front().instruction.kind == Kind::stop)) {
    return result;
  }
  return {};
}

std::optional<unsigned> flag_writes(const Instruction &instruction, const Memory &memory) {
  const auto kind = instruction.kind;
  if (kind == Kind::nop) { return 0; }
  if (kind == Kind::multiply_divide) {
    if (instruction.math == MathOp::multiply_byte) { return flag_c; }
    if (instruction.math == MathOp::multiply_signed ||
        instruction.math == MathOp::multiply_unsigned) {
      return flag_n | flag_z | flag_c;
    }
    return std::nullopt;
  }
  if (kind == Kind::register_operation) {
    const auto operation = instruction.operation;
    if (operation == RegisterOp::index_increment || operation == RegisterOp::index_decrement) {
      return flag_z;
    }
    if (operation == RegisterOp::increment || operation == RegisterOp::decrement) {
      return flags_nzv;
    }
    if (operation == RegisterOp::negate || operation == RegisterOp::complement ||
        operation == RegisterOp::clear || operation == RegisterOp::test ||
        operation == RegisterOp::arithmetic_left || operation == RegisterOp::arithmetic_right ||
        operation == RegisterOp::logical_right) {
      return flags_nzvc;
    }
    return std::nullopt;
  }
  const bool arithmetic = kind == Kind::load || kind == Kind::store || kind == Kind::add ||
                          kind == Kind::subtract || kind == Kind::compare;
  const bool logical = kind == Kind::bit_and || kind == Kind::bit_or || kind == Kind::bit_xor ||
                       kind == Kind::bit_test;
  if ((!arithmetic && !logical) || may_access_owner(instruction, memory)) { return std::nullopt; }
  return written_flags(instruction);
}

std::vector<EmissionOptions> emission_options(const std::span<const BlockInstruction> instructions,
                                              const Memory &memory) {
  std::vector<EmissionOptions> result(instructions.size());
  constexpr unsigned all_flags = 0xff;
  unsigned live = all_flags;
  for (auto index = instructions.size(); index != 0; --index) {
    const auto changed = flag_writes(instructions[index - 1].instruction, memory);
    if (!changed) {
      live = all_flags;
      continue;
    }
    result[index - 1] = {.flags_live = (live & *changed) != 0, .advance_pc = false};
    live &= ~*changed;
  }
  return result;
}

std::optional<ResidentPlan>
resident_prefix_plan(const std::span<const BlockInstruction> instructions, const Memory &memory) {
  const auto boundary = std::ranges::find_if(instructions, [&memory](const auto &operation) {
    return !flag_writes(operation.instruction, memory).has_value();
  });
  const auto count = static_cast<std::size_t>(boundary - instructions.begin());
  if (count == 0 || count == instructions.size()) { return std::nullopt; }
  auto plan = resident_plan(instructions.first(count), memory);
  if (!plan || plan->loop) { return std::nullopt; }
  return plan;
}
}
