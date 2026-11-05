// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/resident.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

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

bool indexed_load(const Instruction &instruction) {
  return instruction.kind == Kind::load && instruction.word &&
         instruction.target == WordTarget::d && instruction.addressing == Addressing::indexed &&
         instruction.indexed.base == IndexBase::x &&
         instruction.indexed.offset == IndexOffset::constant && instruction.indexed.update == 2 &&
         instruction.indexed.displacement == 0 && !instruction.indexed.indirect;
}

bool indexed_memory(ResidentPlan &plan, const Memory &memory) {
  unsigned accesses = 0;
  for (const auto &operation : plan.operations) {
    if (indexed_load(operation.instruction)) {
      ++accesses;
    } else if ((written_words(operation.instruction) & (1U << 1U)) != 0) {
      return false;
    }
  }
  if (accesses != 1 || !plan.loop) { return false; }
  for (const auto &region : memory.regions()) {
    if (region.known.empty() && region.bytes.size() > plan.indexed_region.bytes.size()) {
      plan.indexed_region = region;
    }
  }
  if (plan.indexed_region.bytes.empty()) { return false; }
  plan.base = plan.indexed_region.bytes.data();
  plan.used_words |= 1U << 1U;
  plan.dirty_words |= 1U << 1U;
  return true;
}

bool supported(const Instruction &instruction) {
  if (instruction.kind == Kind::nop || indexed_load(instruction)) { return true; }
  if (instruction.kind == Kind::multiply_divide) {
    return instruction.math == MathOp::multiply_signed ||
           instruction.math == MathOp::multiply_unsigned;
  }
  if (instruction.kind == Kind::branch) {
    constexpr unsigned carry_clear = 4;
    constexpr unsigned carry_set = 5;
    constexpr unsigned not_equal = 6;
    constexpr unsigned equal = 7;
    return instruction.condition < 2 || instruction.condition == carry_clear ||
           instruction.condition == carry_set || instruction.condition == not_equal ||
           instruction.condition == equal;
  }
  if (instruction.kind == Kind::loop) {
    return instruction.word && instruction.condition == 1 &&
           (instruction.target == WordTarget::x || instruction.target == WordTarget::y) &&
           (instruction.operation == RegisterOp::increment ||
            instruction.operation == RegisterOp::decrement);
  }
  if (instruction.kind == Kind::register_operation) {
    return instruction.operation == RegisterOp::index_increment ||
           instruction.operation == RegisterOp::index_decrement ||
           (instruction.word && instruction.target == WordTarget::d &&
            instruction.operation == RegisterOp::arithmetic_left);
  }
  const auto kind = instruction.kind;
  const bool arithmetic = kind == Kind::load || kind == Kind::store || kind == Kind::add ||
                          kind == Kind::subtract || kind == Kind::compare;
  const bool logical = !instruction.word && (kind == Kind::bit_and || kind == Kind::bit_or ||
                                             kind == Kind::bit_xor || kind == Kind::bit_test);
  return (arithmetic || logical) && (instruction.addressing == Addressing::immediate ||
                                     instruction.addressing == Addressing::direct ||
                                     instruction.addressing == Addressing::extended);
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

bool prepare_memory(ResidentPlan &plan, const Memory &memory) {
  bool indexed = false;
  for (const auto &operation : plan.operations) { indexed |= indexed_load(operation.instruction); }
  if (indexed && !indexed_memory(plan, memory)) { return false; }
  for (auto &operation : plan.operations) {
    if (!displacement(operation, plan.base)) { return false; }
  }
  return true;
}

bool induction(ResidentPlan &plan, const Address target) {
  constexpr unsigned not_equal = 6;
  if (target != plan.operations.front().pc) {
    return plan.operations.back().instruction.kind != Kind::loop;
  }
  const auto &last = plan.operations.back();
  const bool counted = last.instruction.kind == Kind::loop;
  if (last.instruction.condition != (counted ? 1U : not_equal)) { return false; }
  const ResidentOperation *counter = counted ? &last : nullptr;
  for (const auto &operation : plan.operations) {
    if (!counted && (written_flags(operation.instruction) & flag_z) != 0) { counter = &operation; }
  }
  if (counter == nullptr || (!counted && counter->instruction.kind != Kind::register_operation)) {
    return false;
  }
  if (!counted && counter->instruction.operation != RegisterOp::index_increment &&
      counter->instruction.operation != RegisterOp::index_decrement) {
    return false;
  }
  const auto word = native_word(counter->instruction);
  for (const auto &operation : plan.operations) {
    if (&operation != counter && (written_words(operation.instruction) & (1U << word)) != 0) {
      return false;
    }
  }
  plan.loop = true;
  plan.increment = counter->instruction.operation == RegisterOp::index_increment ||
                   counter->instruction.operation == RegisterOp::increment;
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
  if (instruction.word) {
    return lane_a | lane_b | (instruction.kind == Kind::multiply_divide ? (1U << 2U) : 0U);
  }
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

bool cacheable_operand(const ResidentPlan &plan, const ResidentOperation &operand) {
  const auto origin = reinterpret_cast<std::uintptr_t>(operand.pointer);
  return std::ranges::all_of(plan.operations, [origin, &operand](const auto &operation) {
    if (operation.instruction.kind != Kind::store) { return true; }
    const auto address = reinterpret_cast<std::uintptr_t>(operation.pointer);
    const auto width = operation.instruction.word ? 2U : 1U;
    const bool overlap = address >= origin ? address - origin < 2 : origin - address < width;
    return !overlap || (operation.pointer == operand.pointer && operation.instruction.word);
  });
}

void forward_operand(ResidentPlan &plan) {
  if (!plan.loop) { return; }
  for (auto &operation : plan.operations) {
    const auto kind = operation.instruction.kind;
    if (!operation.instruction.word || operation.pointer == nullptr ||
        (kind != Kind::add && kind != Kind::subtract && kind != Kind::compare) ||
        !cacheable_operand(plan, operation)) {
      continue;
    }
    operation.cached_operand = true;
    for (auto &store : plan.operations) {
      if (store.instruction.kind == Kind::store && store.pointer == operation.pointer) {
        store.cache_store = true;
      }
    }
    return;
  }
}

void analyze(ResidentPlan &plan) {
  unsigned live = flags_nzvc | flag_h;
  unsigned overwritten = 0;
  const auto &last = plan.operations.back().instruction;
  constexpr unsigned not_equal = 6;
  constexpr unsigned equal = 7;
  bool last_zero =
      last.kind == Kind::branch && (last.condition == not_equal || last.condition == equal);
  for (auto position = plan.operations.rbegin(); position != plan.operations.rend(); ++position) {
    const auto &instruction = position->instruction;
    const auto changed = written_flags(instruction);
    const auto writes = written_words(instruction);
    position->flags = live & changed;
    const bool native_result = instruction.kind == Kind::multiply_divide ||
                               (instruction.kind == Kind::register_operation &&
                                instruction.operation == RegisterOp::arithmetic_left);
    const bool stable_flags =
        !native_result && (position->pointer == nullptr || instruction.kind == Kind::load ||
                           instruction.kind == Kind::store);
    position->deferred =
        stable_flags && position->flags != 0 && (value_lanes(instruction) & overwritten) == 0;
    position->branch_zero = last_zero && (changed & flag_z) != 0;
    if ((changed & flag_z) != 0) { last_zero = false; }
    if (writes != 0) {
      overwritten |= value_lanes(instruction) | (indexed_load(instruction) ? (1U << 1U) : 0U);
    }
    if (instruction.kind != Kind::nop && instruction.kind != Kind::branch) {
      plan.used_words |= (1U << native_word(instruction)) |
                         (instruction.kind == Kind::multiply_divide ? (1U << 2U) : 0U);
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
  if (kind == Kind::multiply_divide) { return flag_n | flag_z | flag_c; }
  if (kind == Kind::register_operation && instruction.operation == RegisterOp::arithmetic_left) {
    return flags_nzvc;
  }
  if (kind == Kind::add) { return flags_nzvc | (instruction.word ? 0U : flag_h); }
  if (kind == Kind::subtract || kind == Kind::compare) { return flags_nzvc; }
  if (kind == Kind::load || kind == Kind::store || kind == Kind::bit_and || kind == Kind::bit_or ||
      kind == Kind::bit_xor || kind == Kind::bit_test) {
    return flags_nzv;
  }
  return kind == Kind::register_operation ? flag_z : 0U;
}

unsigned written_words(const Instruction &instruction) {
  if (instruction.kind == Kind::multiply_divide) { return (1U << 3U) | (1U << 2U); }
  if (instruction.kind == Kind::load || instruction.kind == Kind::add ||
      instruction.kind == Kind::subtract || instruction.kind == Kind::bit_and ||
      instruction.kind == Kind::bit_or || instruction.kind == Kind::bit_xor ||
      instruction.kind == Kind::register_operation || instruction.kind == Kind::loop) {
    return (1U << native_word(instruction)) | (indexed_load(instruction) ? (1U << 1U) : 0U);
  }
  return 0;
}

std::optional<ResidentPlan> resident_plan(const std::span<const BlockInstruction> instructions,
                                          const Memory &memory) {
  if (instructions.empty()) { return std::nullopt; }
  ResidentPlan plan;
  for (const auto &operation : instructions) {
    auto instruction = operation.instruction;
    if (instruction.kind == Kind::multiply_divide) { instruction.word = true; }
    if (!supported(instruction)) { return std::nullopt; }
    const Byte *data = nullptr;
    const bool access = instruction.addressing == Addressing::direct ||
                        instruction.addressing == Addressing::extended;
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
  const auto &last = plan.operations.back();
  if (last.instruction.kind == Kind::branch || last.instruction.kind == Kind::loop) {
    const auto target = static_cast<Address>(static_cast<int>(last.pc) + last.instruction.length +
                                             std::bit_cast<std::int16_t>(last.instruction.operand));
    if (!induction(plan, target)) { return std::nullopt; }
  }
  if (!prepare_memory(plan, memory)) { return std::nullopt; }
  fold(plan);
  analyze(plan);
  forward_load(plan);
  forward_operand(plan);
  return plan;
}

std::optional<ResidentPlan> resident_loop_plan(const std::span<const BlockInstruction> instructions,
                                               const Memory &memory) {
  if (instructions.empty()) { return std::nullopt; }
  const auto &last = instructions.back();
  constexpr unsigned equal = 7;
  constexpr unsigned not_equal = 6;
  if (last.instruction.kind != Kind::branch || last.instruction.condition != equal) {
    return std::nullopt;
  }
  const auto next = static_cast<Address>(last.pc + last.instruction.length);
  const auto backedge = parse_block(memory, next);
  if (backedge.size() != 1) { return std::nullopt; }
  const auto &jump = backedge.front().instruction;
  const auto after = static_cast<Address>(next + jump.length);
  const auto exit =
      static_cast<Address>(int{next} + std::bit_cast<std::int16_t>(last.instruction.operand));
  if (exit != after) { return std::nullopt; }
  Address target = 0;
  if (jump.kind == Kind::branch && jump.condition == 0) {
    target = static_cast<Address>(int{after} + std::bit_cast<std::int16_t>(jump.operand));
  } else if (jump.kind == Kind::jump && jump.addressing == Addressing::extended) {
    target = jump.address;
  } else {
    return std::nullopt;
  }
  if (target != instructions.front().pc) { return std::nullopt; }
  // BEQ exit; BRA/JMP header becomes one native BNE header. The original BEQ
  // remains the last retired guest instruction on the final iteration.
  std::vector<BlockInstruction> combined(instructions.begin(), instructions.end());
  auto &branch = combined.back().instruction;
  branch.condition = not_equal;
  branch.length = static_cast<Byte>(branch.length + jump.length);
  branch.operand = static_cast<Address>(int{target} - int{after});
  auto plan = resident_plan(combined, memory);
  if (!plan || !plan->loop) { return std::nullopt; }
  plan->backedge_instructions = 1;
  plan->backedge_exit = {.pc = next, .opcode = jump.opcode};
  return plan;
}

}
