// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/resident.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace cpu12::detail {
namespace {
const Byte *pointer(const Memory &memory, const Instruction &instruction) {
  for (const auto &region : memory.regions()) {
    if (!region.known.empty() || instruction.address < region.first ||
        (instruction.kind == Kind::store && !region.writable)) {
      continue;
    }
    const auto offset = static_cast<std::size_t>(instruction.address - region.first);
    const auto width = instruction.word ? 2U : 1U;
    if (offset <= region.bytes.size() && width <= region.bytes.size() - offset) {
      return region.bytes.data() + offset;
    }
  }
  return nullptr;
}

bool supported(const Instruction &instruction) {
  if (instruction.kind == Kind::nop) { return true; }
  if (instruction.kind == Kind::branch) {
    constexpr unsigned not_equal = 6;
    constexpr unsigned equal = 7;
    return instruction.condition < 2 || instruction.condition == not_equal ||
           instruction.condition == equal;
  }
  if (instruction.kind == Kind::register_operation) {
    return instruction.operation == RegisterOp::index_increment ||
           instruction.operation == RegisterOp::index_decrement;
  }
  const auto kind = instruction.kind;
  const bool arithmetic = kind == Kind::load || kind == Kind::store || kind == Kind::add ||
                          kind == Kind::subtract || kind == Kind::compare;
  const bool logical = !instruction.word && (kind == Kind::bit_and || kind == Kind::bit_or ||
                                             kind == Kind::bit_xor || kind == Kind::bit_test);
  return (arithmetic || logical) && (instruction.addressing == Addressing::immediate ||
                                     ((kind == Kind::load || kind == Kind::store) &&
                                      (instruction.addressing == Addressing::direct ||
                                       instruction.addressing == Addressing::extended)));
}

bool displacement(ResidentOperation &operation, const Byte *base) {
  if (operation.pointer == nullptr) { return true; }
  const auto value = reinterpret_cast<std::uintptr_t>(operation.pointer);
  const auto origin = reinterpret_cast<std::uintptr_t>(base);
  constexpr auto limit = static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max());
  if (value >= origin) {
    if (value - origin > limit) { return false; }
    operation.displacement = static_cast<std::int32_t>(value - origin);
  } else {
    if (origin - value > limit) { return false; }
    operation.displacement = -static_cast<std::int32_t>(origin - value);
  }
  return true;
}

bool induction(ResidentPlan &plan, const Address target) {
  constexpr unsigned not_equal = 6;
  if (target != plan.operations.front().pc) { return true; }
  if (plan.operations.back().instruction.condition != not_equal) { return false; }
  const ResidentOperation *counter = nullptr;
  for (const auto &operation : plan.operations) {
    if ((written_flags(operation.instruction) & flag_z) != 0) { counter = &operation; }
  }
  if (counter == nullptr || counter->instruction.kind != Kind::register_operation) { return false; }
  const auto word = native_word(counter->instruction);
  for (const auto &operation : plan.operations) {
    if (&operation != counter && (written_words(operation.instruction) & (1U << word)) != 0) {
      return false;
    }
  }
  plan.loop = true;
  plan.increment = counter->instruction.operation == RegisterOp::index_increment;
  plan.counter_offset = counter->instruction.target == WordTarget::x ? offsetof(Registers, x)
                                                                     : offsetof(Registers, y);
  return true;
}

unsigned value_lanes(const Instruction &instruction) {
  constexpr unsigned d = 3;
  constexpr unsigned lane_a = 1U << 3U;
  constexpr unsigned lane_b = 1U << 4U;
  const auto word = native_word(instruction);
  if (word != d) { return 1U << word; }
  if (instruction.word) { return lane_a | lane_b; }
  return instruction.accumulator_b ? lane_b : lane_a;
}

void fold(ResidentPlan &plan) {
  for (std::size_t index = 1; index < plan.operations.size(); ++index) {
    auto &left = plan.operations[index - 1].instruction;
    auto &right = plan.operations[index].instruction;
    if (left.kind == Kind::bit_xor && right.kind == Kind::bit_xor &&
        left.addressing == Addressing::immediate && right.addressing == Addressing::immediate &&
        left.accumulator_b == right.accumulator_b && left.operand == right.operand) {
      // XOR k twice restores the value; only the second instruction's flags survive.
      left.kind = Kind::nop;
      right.kind = Kind::bit_test;
      constexpr unsigned byte_mask = 0xff;
      right.operand = byte_mask;
    }
  }
}

void forward_load(ResidentPlan &plan) {
  if (!plan.loop) { return; }
  ResidentOperation *load = nullptr;
  const ResidentOperation *store = nullptr;
  for (auto &operation : plan.operations) {
    if (operation.instruction.kind == Kind::nop) { continue; }
    load = &operation;
    break;
  }
  if (load == nullptr || load->instruction.kind != Kind::load || load->pointer == nullptr ||
      load->flags != 0) {
    return;
  }
  const auto origin = reinterpret_cast<std::uintptr_t>(load->pointer);
  const auto width = load->instruction.word ? 2U : 1U;
  for (const auto &operation : plan.operations) {
    if (operation.instruction.kind != Kind::store) { continue; }
    const auto address = reinterpret_cast<std::uintptr_t>(operation.pointer);
    const auto written = operation.instruction.word ? 2U : 1U;
    const bool overlap = address >= origin ? address - origin < width : origin - address < written;
    if (overlap) { store = &operation; }
  }
  if (store == nullptr || store->pointer != load->pointer ||
      store->instruction.word != load->instruction.word ||
      value_lanes(store->instruction) != value_lanes(load->instruction)) {
    return;
  }
  bool after_store = false;
  for (const auto &operation : plan.operations) {
    if (after_store && written_words(operation.instruction) != 0 &&
        (value_lanes(operation.instruction) & value_lanes(load->instruction)) != 0) {
      return;
    }
    after_store |= &operation == store;
  }
  // First iteration still reads real RAM; each later iteration forwards the last store.
  load->initial_load = true;
}

void analyze(ResidentPlan &plan) {
  unsigned live = flags_nzvc | flag_h;
  unsigned overwritten = 0;
  bool last_zero = plan.operations.back().instruction.kind == Kind::branch;
  for (auto position = plan.operations.rbegin(); position != plan.operations.rend(); ++position) {
    const auto &instruction = position->instruction;
    const auto changed = written_flags(instruction);
    const auto writes = written_words(instruction);
    position->flags = live & changed;
    position->deferred = position->flags != 0 && (value_lanes(instruction) & overwritten) == 0;
    position->branch_zero = last_zero && (changed & flag_z) != 0;
    if ((changed & flag_z) != 0) { last_zero = false; }
    if (writes != 0) { overwritten |= value_lanes(instruction); }
    if (instruction.kind != Kind::nop && instruction.kind != Kind::branch) {
      plan.used_words |= 1U << native_word(instruction);
      plan.dirty_words |= writes;
    }
    live &= ~changed;
    plan.changed |= changed;
    plan.host_zero |= (changed & flag_z) != 0;
  }
}
}

unsigned native_word(const Instruction &instruction) {
  constexpr unsigned native_bx = 3;
  constexpr unsigned native_cx = 1;
  constexpr unsigned native_dx = 2;
  constexpr unsigned native_bp = 5;
  if (!instruction.word && instruction.kind != Kind::register_operation) { return native_bx; }
  switch (instruction.target) {
    case WordTarget::d: return native_bx;
    case WordTarget::x: return native_cx;
    case WordTarget::y: return native_dx;
    case WordTarget::sp: return native_bp;
    default: return native_bx;
  }
}

unsigned written_flags(const Instruction &instruction) {
  const auto kind = instruction.kind;
  if (kind == Kind::add) { return flags_nzvc | (instruction.word ? 0U : flag_h); }
  if (kind == Kind::subtract || kind == Kind::compare) { return flags_nzvc; }
  if (kind == Kind::load || kind == Kind::store || kind == Kind::bit_and || kind == Kind::bit_or ||
      kind == Kind::bit_xor || kind == Kind::bit_test) {
    return flags_nzv;
  }
  return kind == Kind::register_operation ? flag_z : 0U;
}

unsigned written_words(const Instruction &instruction) {
  if (instruction.kind == Kind::load || instruction.kind == Kind::add ||
      instruction.kind == Kind::subtract || instruction.kind == Kind::bit_and ||
      instruction.kind == Kind::bit_or || instruction.kind == Kind::bit_xor ||
      instruction.kind == Kind::register_operation) {
    return 1U << native_word(instruction);
  }
  return 0;
}

std::optional<ResidentPlan> resident_plan(const std::span<const BlockInstruction> instructions,
                                          const Memory &memory) {
  if (instructions.empty()) { return std::nullopt; }
  ResidentPlan plan;
  for (const auto &operation : instructions) {
    const auto &instruction = operation.instruction;
    if (!supported(instruction)) { return std::nullopt; }
    const Byte *data = nullptr;
    const bool access = (instruction.kind == Kind::load || instruction.kind == Kind::store) &&
                        instruction.addressing != Addressing::immediate;
    if (access) {
      data = pointer(memory, instruction);
      if (data == nullptr) { return std::nullopt; }
    }
    if (plan.base == nullptr && data != nullptr) { plan.base = data; }
    plan.operations.push_back({.instruction = instruction, .pc = operation.pc, .pointer = data});
    if (instruction.word && instruction.target == WordTarget::d) { plan.swapped_d = true; }
  }
  // Byte-only regions put A in BL and B in BH; a word D region keeps native D order.
  plan.swapped_d = !plan.swapped_d;
  for (auto &operation : plan.operations) {
    if (!displacement(operation, plan.base)) { return std::nullopt; }
  }
  const auto &last = plan.operations.back();
  if (last.instruction.kind == Kind::branch) {
    const auto target = static_cast<Address>(static_cast<int>(last.pc) + last.instruction.length +
                                             std::bit_cast<std::int16_t>(last.instruction.operand));
    if (!induction(plan, target)) { return std::nullopt; }
  }
  fold(plan);
  analyze(plan);
  forward_load(plan);
  return plan;
}
}
