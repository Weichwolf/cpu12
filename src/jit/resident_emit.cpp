// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/code.hpp"
#include "jit/resident.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpu12::detail {
namespace {
constexpr unsigned byte_bits = 8, native_high = 4;

enum NativeByte : std::uint8_t {
  rex_base = 0x40,
  rex_b = 0x41,
  rex_r = 0x44,
  escape = 0x0f,
  register_operand = 0xc0,
  r15_displacement = 0x87,
  rdi_displacement = 0x47,
  sub8 = 0x28,
  cmp8 = 0x38,
  xor8 = 0x30,
  mov8_to = 0x88,
  mov8_from = 0x8a,
  mov_to = 0x89,
  mov_from = 0x8b,
  test8 = 0x84,
  test_word = 0x85,
  word_immediate = 0x81,
  byte_immediate = 0x80,
  mov8_immediate = 0xb0,
  mov_word_immediate = 0xb8,
  test_immediate = 0xf6,
  and_eax = 0x25,
  shift_immediate = 0xc1,
  shift_left_eax = 0xe0,
  shift_right_eax = 0xe8,
  jump_zero = 0x84,
  jump_nonzero = 0x85
};

constexpr std::uint8_t word_prefix = 0x66;
constexpr std::array<std::uint8_t, 5> snapshots{6, 8, 9, 10, 11};
constexpr std::array<unsigned, 4> word_offsets{offsetof(Registers, d),
                                               offsetof(Registers, x),
                                               offsetof(Registers, y),
                                               offsetof(Registers, sp)};
constexpr std::array<unsigned, 4> word_registers{3, 1, 2, 5};

struct FlagValue {
  unsigned mask = 0;
  std::uint8_t reg = 0;
  bool packed = false, boolean = false;
};

enum class LoopMode : std::uint8_t { full, bounded };

struct EmissionState {
  std::vector<FlagValue> flags;
};

void rex(Code &code, const std::uint8_t reg) {
  constexpr unsigned legacy_count = 8;
  if (reg >= legacy_count) { code.byte(rex_b); }
}

void preserve(Code &code, const ResidentPlan &plan, const bool restore, const LoopMode mode) {
  constexpr std::array<std::uint8_t, 5> registers{3, 5, 12, 13, 15};
  const std::array needed{(plan.used_words & (1U << registers[0])) != 0,
                          (plan.used_words & (1U << registers[1])) != 0,
                          plan.changed != 0,
                          mode == LoopMode::bounded,
                          plan.base != nullptr};
  constexpr std::uint8_t push = 0x50;
  constexpr std::uint8_t pop = 0x58;
  for (std::size_t offset = 0; offset < registers.size(); ++offset) {
    const auto index = restore ? registers.size() - 1 - offset : offset;
    if (!needed[index]) { continue; }
    rex(code, registers[index]);
    code.byte(static_cast<std::uint8_t>((restore ? pop : push) + (registers[index] & 7U)));
  }
}

void set_flag(Code &code, const FlagValue flag) {
  constexpr std::array<std::uint8_t, 4> codes{0x92, 0x90, 0x94, 0x98};
  const auto opcode = codes[static_cast<std::size_t>(std::countr_zero(flag.mask))];
  const auto reg = flag.reg;
  // SIL requires a REX prefix even though its register index is below eight.
  code.byte(reg < snapshots[1] ? rex_base : rex_b);
  code.byte(escape);
  code.byte(opcode);
  code.byte(static_cast<std::uint8_t>(register_operand | (reg & 7U)));
}

void capture(Code &code, const ResidentOperation &operation, EmissionState &state) {
  const auto arithmetic = operation.instruction.kind == Kind::add ||
                          operation.instruction.kind == Kind::subtract ||
                          operation.instruction.kind == Kind::compare;
  const auto mask = operation.flags & (arithmetic ? ~0U : ~flag_v);
  if (mask == 0) { return; }
  const auto reg = snapshots[state.flags.size()];
  const bool boolean = std::has_single_bit(mask) && mask != flag_h;
  const bool packed = (mask & flag_v) != 0 && !boolean;
  if (boolean) {
    set_flag(code, {.mask = mask, .reg = reg});
  } else {
    constexpr std::uint8_t lahf = 0x9f;
    code.byte(lahf);
    if (packed) {
      constexpr std::array<std::uint8_t, 3> overflow{0x0f, 0x90, 0xc0};
      code.raw(overflow);
    } else {
      constexpr std::array<std::uint8_t, 3> high_byte{0x0f, 0xb6, 0xc4};
      code.raw(high_byte);
    }
    if (reg >= snapshots[1]) { code.byte(rex_r); }
    if (packed) {
      constexpr std::array<std::uint8_t, 2> extend{0x0f, 0xb7};
      code.raw(extend);
    } else {
      code.byte(mov_from);
    }
    code.byte(static_cast<std::uint8_t>(register_operand | ((reg & 7U) << 3U)));
  }
  state.flags.push_back({.mask = mask, .reg = reg, .packed = packed, .boolean = boolean});
}

void swap_word(Code &code, const unsigned reg) {
  constexpr std::array<std::uint8_t, 2> rotate{0x66, 0xc1};
  code.raw(rotate);
  code.byte(static_cast<std::uint8_t>(register_operand | reg));
  code.byte(byte_bits);
}

unsigned byte_register(const Instruction &instruction, const ResidentPlan &plan) {
  constexpr unsigned bx = 3;
  const bool high = plan.swapped_d ? instruction.accumulator_b : !instruction.accumulator_b;
  return bx + (high ? native_high : 0U);
}

void test_value(Code &code, const Instruction &instruction, const ResidentPlan &plan) {
  const auto reg = instruction.word ? native_word(instruction) : byte_register(instruction, plan);
  if (instruction.word) { code.byte(word_prefix); }
  code.byte(instruction.word ? test_word : test8);
  code.byte(static_cast<std::uint8_t>(register_operand | (reg << 3U) | reg));
}

void indexed_operation(Code &code,
                       const ResidentOperation &operation,
                       const ResidentPlan &plan,
                       const bool needed_flags) {
  // Admission proves that ECX stays in range until the final increment.
  constexpr std::array<std::uint8_t, 5> load{0x41, 0x0f, 0xb7, 0x9c, 0x0f}; // EBX,[R15+RCX+disp].
  code.raw(load);
  code.u32(std::bit_cast<std::uint32_t>(-static_cast<std::int32_t>(plan.indexed_region.first)));
  swap_word(code, 3);
  constexpr std::array<std::uint8_t, 3> advance{0x8d, 0x49, 0x02}; // LEA ECX,[RCX+2].
  code.raw(advance);
  if (needed_flags) { test_value(code, operation.instruction, plan); }
}

void memory_arithmetic(Code &code, const ResidentOperation &operation, const ResidentPlan &plan) {
  const auto &instruction = operation.instruction;
  const auto reg = instruction.word ? native_word(instruction) : byte_register(instruction, plan);
  if (!operation.cached_operand) {
    if (instruction.word) { code.byte(word_prefix); }
    code.byte(rex_b);
    code.byte(instruction.word ? mov_from : mov8_from);
    code.byte(r15_displacement); // Read the actual current operand into AX/AL.
    code.u32(std::bit_cast<std::uint32_t>(operation.displacement));
    if (instruction.word) { swap_word(code, 0); }
  }
  std::uint8_t opcode = 0;
  if (instruction.kind == Kind::subtract) {
    opcode = sub8;
  } else if (instruction.kind == Kind::compare) {
    opcode = cmp8;
  } else if (instruction.kind == Kind::bit_or) {
    opcode = 0x08;
  } else if (instruction.kind == Kind::bit_and) {
    opcode = 0x20;
  } else if (instruction.kind == Kind::bit_xor) {
    opcode = xor8;
  } else if (instruction.kind == Kind::bit_test) {
    opcode = test8;
  } else if (instruction.kind != Kind::add) {
    throw std::logic_error("Unsupported resident memory arithmetic");
  }
  if (instruction.word) {
    code.byte(word_prefix);
    ++opcode;
  }
  if (operation.cached_operand) { code.byte(rex_r); }
  code.byte(opcode);
  const auto source = operation.cached_operand ? 4U : 0U; // R12 or AX.
  code.byte(static_cast<std::uint8_t>(register_operand | (source << 3U) | reg));
}

void memory_operation(Code &code,
                      const ResidentOperation &operation,
                      const ResidentPlan &plan,
                      const bool needed_flags) {
  const auto &instruction = operation.instruction;
  if (instruction.kind != Kind::load && instruction.kind != Kind::store) {
    memory_arithmetic(code, operation, plan);
    return;
  }
  const bool store = instruction.kind == Kind::store;
  auto reg = instruction.word ? native_word(instruction) : byte_register(instruction, plan);
  const bool high = !instruction.word && reg >= native_high;
  const auto value_register = reg;
#if !defined(__MOVBE__)
  if (instruction.word && store) {
    // Convert a scratch copy so later operations can use the unchanged guest value.
    code.byte(word_prefix);
    code.byte(mov_to);
    code.byte(static_cast<std::uint8_t>(register_operand | (reg << 3U)));
    swap_word(code, 0);
    reg = 0;
  }
#endif
  if (high && store) {
    code.byte(mov8_to);
    code.byte(static_cast<std::uint8_t>(register_operand | (reg << 3U)));
  }
  if (high) { reg = 0; }
  if (instruction.word) { code.byte(word_prefix); }
  code.byte(rex_b); // R15 is the fixed memory base; displacement remains compile-time data.
  constexpr std::array<std::array<std::uint8_t, 2>, 2> opcodes{{{0x8a, 0x88}, {0x8b, 0x89}}};
#if defined(__MOVBE__)
  if (instruction.word && store) {
    constexpr std::array<std::uint8_t, 3> movbe_store{0x0f, 0x38, 0xf1};
    code.raw(movbe_store);
  } else
#endif
  {
    code.byte(opcodes[static_cast<std::size_t>(instruction.word)][static_cast<std::size_t>(store)]);
  }
  code.byte(static_cast<std::uint8_t>(r15_displacement | (reg << 3U)));
  code.u32(std::bit_cast<std::uint32_t>(operation.displacement));
  if (high && !store) {
    code.byte(mov8_to);
    code.byte(static_cast<std::uint8_t>(register_operand | byte_register(instruction, plan)));
  }
  if (instruction.word && !store) { swap_word(code, reg); }
  if (needed_flags) { test_value(code, instruction, plan); }
  if (operation.cache_store) {
    constexpr std::array<std::uint8_t, 3> cache{word_prefix, rex_b, mov_to};
    code.raw(cache);
    code.byte(static_cast<std::uint8_t>(register_operand | (value_register << 3U) | 4U));
  }
}

void initial_operand(Code &code, const ResidentOperation &operation) {
  constexpr std::array<std::uint8_t, 4> load{word_prefix, rex_b, mov_from, r15_displacement};
  code.raw(load);
  code.u32(std::bit_cast<std::uint32_t>(operation.displacement));
  swap_word(code, 0);
  constexpr std::array<std::uint8_t, 4> cache{word_prefix, rex_b, mov_to, 0xc4}; // R12W,AX.
  code.raw(cache);
}

void immediate_operation(Code &code,
                         const Instruction &instruction,
                         const ResidentPlan &plan,
                         const bool needed_flags = true) {
  const auto reg = instruction.word ? native_word(instruction) : byte_register(instruction, plan);
  if (instruction.word) { code.byte(word_prefix); }
  if (instruction.kind == Kind::load) {
    code.byte(
        static_cast<std::uint8_t>((instruction.word ? mov_word_immediate : mov8_immediate) + reg));
  } else {
    unsigned group = 7;
    if (instruction.kind == Kind::add || instruction.kind == Kind::bit_test) { group = 0; }
    if (instruction.kind == Kind::bit_or) { group = 1; }
    if (instruction.kind == Kind::bit_and) { group = 4; }
    if (instruction.kind == Kind::subtract) { group = 5; }
    if (instruction.kind == Kind::bit_xor) { group = 6; }
    auto opcode = byte_immediate;
    if (instruction.word) { opcode = word_immediate; }
    if (instruction.kind == Kind::bit_test) { opcode = test_immediate; }
    code.byte(opcode);
    code.byte(static_cast<std::uint8_t>(register_operand | (group << 3U) | reg));
  }
  if (instruction.word) {
    code.u16(instruction.operand);
  } else {
    code.byte(static_cast<std::uint8_t>(instruction.operand));
  }
  if (instruction.kind == Kind::load && needed_flags) { test_value(code, instruction, plan); }
}

void body(Code &code, const ResidentPlan &plan, EmissionState &state) {
  for (const auto &operation : plan.operations) {
    const auto &instruction = operation.instruction;
    if (instruction.kind == Kind::nop || instruction.kind == Kind::branch ||
        operation.initial_load) {
      continue;
    }
    const bool needed_flags =
        (!operation.deferred && operation.flags != 0) || operation.branch_zero;
    if ((instruction.kind == Kind::compare || instruction.kind == Kind::bit_test) &&
        !needed_flags) {
      continue;
    }
    if (instruction.kind == Kind::register_operation || instruction.kind == Kind::loop) {
      constexpr std::array<std::uint8_t, 2> increment{0x66, 0xff};
      code.raw(increment);
      const bool incrementing = instruction.operation == RegisterOp::index_increment ||
                                instruction.operation == RegisterOp::increment;
      const unsigned group = incrementing ? 0U : 1U;
      code.byte(
          static_cast<std::uint8_t>(register_operand | (group << 3U) | native_word(instruction)));
    } else if (instruction.addressing == Addressing::indexed) {
      indexed_operation(code, operation, plan, needed_flags);
    } else if (operation.pointer != nullptr) {
      memory_operation(code, operation, plan, needed_flags);
    } else {
      immediate_operation(code, instruction, plan, needed_flags);
    }
    if (!operation.deferred) { capture(code, operation, state); }
  }
}

void deferred_flags(Code &code, const ResidentPlan &plan, EmissionState &state) {
  for (const auto &operation : plan.operations) {
    if (!operation.deferred) { continue; }
    auto instruction = operation.instruction;
    if (instruction.kind == Kind::add || instruction.kind == Kind::subtract) {
      // Reconstruct the producer's input modulo its width, then recreate its flags.
      auto inverse = instruction;
      inverse.kind = instruction.kind == Kind::add ? Kind::subtract : Kind::add;
      immediate_operation(code, inverse, plan);
      immediate_operation(code, instruction, plan);
    } else if (instruction.kind == Kind::compare || instruction.kind == Kind::bit_test) {
      immediate_operation(code, instruction, plan);
    } else {
      if (instruction.kind == Kind::register_operation) { instruction.word = true; }
      test_value(code, instruction, plan);
    }
    capture(code, operation, state);
  }
}

void copy_snapshot(Code &code, const FlagValue flag) {
  if (flag.boolean && flag.reg < snapshots[1]) {
    code.byte(rex_base);
  } else {
    rex(code, flag.reg);
  }
  if (flag.boolean) {
    constexpr std::array<std::uint8_t, 2> extend{0x0f, 0xb6};
    code.raw(extend);
    code.byte(static_cast<std::uint8_t>(register_operand | (flag.reg & 7U)));
  } else {
    code.byte(mov_from);
    code.byte(static_cast<std::uint8_t>(register_operand | (flag.reg & 7U)));
  }
}

void publish_flags(Code &code, const ResidentPlan &plan, const EmissionState &state) {
  constexpr auto ccr = static_cast<std::uint8_t>(offsetof(Registers, ccr));
  constexpr std::array<std::uint8_t, 4> load{0x44, 0x0f, 0xb6, 0x67};
  code.raw(load);
  code.byte(ccr);
  constexpr std::array<std::uint8_t, 3> retain{0x41, 0x81, 0xe4};
  code.raw(retain);
  constexpr unsigned byte_mask = 0xff;
  code.u32(byte_mask & ~plan.changed);
  constexpr std::array<unsigned, 5> bits{flag_c, flag_v, flag_z, flag_n, flag_h};
  for (const auto flag : state.flags) {
    for (const auto &bit : bits) {
      if ((flag.mask & bit) == 0) { continue; }
      copy_snapshot(code, flag);
      constexpr std::array<unsigned, 5> byte_sources{1, 1, 0x40, 0x80, 0x10};
      constexpr std::array<unsigned, 5> packed_sources{0x100, 1, 0x4000, 0x8000, 0x1000};
      unsigned source = 1;
      if (!flag.boolean) {
        const auto index = static_cast<std::size_t>(&bit - bits.data());
        source = flag.packed ? packed_sources[index] : byte_sources[index];
      }
      code.byte(and_eax);
      code.u32(source);
      const auto distance = std::countr_zero(bit) - std::countr_zero(source);
      if (distance != 0) {
        code.byte(shift_immediate);
        code.byte(distance > 0 ? shift_left_eax : shift_right_eax);
        code.byte(static_cast<std::uint8_t>(distance > 0 ? distance : -distance));
      }
      constexpr std::array<std::uint8_t, 3> merge{0x41, 0x09, 0xc4};
      code.raw(merge);
    }
  }
  constexpr std::array<std::uint8_t, 3> store{0x44, 0x88, 0x67};
  code.raw(store);
  code.byte(ccr);
}

void registers(Code &code, const ResidentPlan &plan, const bool store) {
  const auto active = store ? plan.dirty_words : plan.used_words;
  for (std::size_t index = 0; index < word_offsets.size(); ++index) {
    if ((active & (1U << word_registers[index])) == 0) { continue; }
    if (!store && !plan.indexed_region.bytes.empty() &&
        word_offsets[index] == offsetof(Registers, x)) {
      constexpr std::array<std::uint8_t, 2> load_index{0x0f, 0xb7}; // MOVZX ECX,word state.
      code.raw(load_index);
    } else {
      code.byte(word_prefix);
      code.byte(store ? mov_to : mov_from);
    }
    code.byte(static_cast<std::uint8_t>(rdi_displacement | (word_registers[index] << 3U)));
    code.byte(static_cast<std::uint8_t>(word_offsets[index]));
  }
  if (!store && plan.swapped_d && (active & (1U << word_registers[0])) != 0) {
    swap_word(code, word_registers[0]);
  }
}

void pc(Code &code, const Address value) {
  constexpr auto offset = static_cast<std::uint8_t>(offsetof(Registers, pc));
  constexpr std::array<std::uint8_t, 3> store{0x66, 0xc7, 0x47};
  code.raw(store);
  code.byte(offset);
  code.u16(value);
}

void finish_branch(Code &code,
                   const ResidentPlan &plan,
                   const std::size_t loop,
                   const LoopMode mode) {
  const auto &last = plan.operations.back();
  const auto next = static_cast<Address>(last.pc + last.instruction.length);
  if (last.instruction.kind != Kind::branch && last.instruction.kind != Kind::loop) {
    pc(code, next);
    return;
  }
  const auto target =
      static_cast<Address>(next + std::bit_cast<std::int16_t>(last.instruction.operand));
  const auto condition = last.instruction.condition;
  constexpr unsigned not_equal = 6;
  if (plan.loop) {
    if (mode == LoopMode::bounded) {
      constexpr std::array<std::uint8_t, 3> decrement{0x41, 0xff, 0xcd}; // DEC R13D.
      code.raw(decrement);
    }
    constexpr std::array<std::uint8_t, 2> repeat{0x0f, 0x85};
    code.raw(repeat);
    const auto displacement = code.relative32();
    code.bind(displacement, loop);
    pc(code, mode == LoopMode::bounded ? target : next);
  } else if (condition < 2) {
    pc(code, condition == 0 ? target : next);
  } else {
    if (!plan.host_zero) {
      constexpr auto offset = static_cast<std::uint8_t>(offsetof(Registers, ccr));
      constexpr std::array<std::uint8_t, 2> test{0xf6, 0x47};
      code.raw(test);
      code.byte(offset);
      code.byte(flag_z);
    }
    pc(code, next);
    code.byte(escape);
    const bool branch_nonzero = (condition == not_equal) == plan.host_zero;
    code.byte(branch_nonzero ? test8 : test_word);
    const auto skip = code.relative32();
    pc(code, target);
    code.bind(skip);
  }
}

std::vector<std::size_t> indexed_admission(Code &code, const ResidentPlan &plan) {
  if (plan.indexed_region.bytes.empty()) { return {}; }
  std::vector<std::size_t> exits;
  constexpr std::array<std::uint8_t, 4> load{0x0f, 0xb7, 0x47, 0x04}; // Initial X.
  constexpr std::array<std::uint8_t, 2> below{0x0f, 0x82};
  code.raw(load);
  constexpr std::uint8_t subtract_eax = 0x2d;
  code.byte(subtract_eax);
  code.u32(plan.indexed_region.first);
  code.raw(below);
  exits.push_back(code.relative32());
  constexpr std::array<std::uint8_t, 2> capacity{0x41, 0xb8};
  code.raw(capacity);
  code.u32(static_cast<std::uint32_t>(plan.indexed_region.bytes.size()));
  constexpr std::array<std::uint8_t, 3> remaining{0x41, 0x29, 0xc0};
  code.raw(remaining);
  code.raw(below);
  exits.push_back(code.relative32());
  // R10 is the admitted trip count, independent of retirement weighting.
  constexpr std::array<std::uint8_t, 7> extent{0x47, 0x8d, 0x0c, 0x12, 0x45, 0x39, 0xc8};
  code.raw(extent);
  code.raw(below);
  exits.push_back(code.relative32());
  return exits;
}

void trip_count(Code &code, const ResidentPlan &plan) {
  constexpr std::array<std::uint8_t, 4> load{0x44, 0x0f, 0xb7, 0x57};
  code.raw(load);
  code.byte(static_cast<std::uint8_t>(plan.counter_offset));
  if (plan.increment) {
    constexpr std::array<std::uint8_t, 4> negate{0x66, 0x41, 0xf7, 0xda};
    code.raw(negate);
  }
  // Zero means a complete 16-bit wrap, not zero executions.
  constexpr std::array<std::uint8_t, 3> test{0x45, 0x85, 0xd2};
  code.raw(test);
  constexpr std::array<std::uint8_t, 2> nonzero{0x0f, 0x85};
  code.raw(nonzero);
  const auto finite = code.relative32();
  constexpr std::array<std::uint8_t, 2> full_wrap{0x41, 0xba};
  code.raw(full_wrap);
  constexpr unsigned word_values = 65536;
  code.u32(word_values);
  code.bind(finite);
}

std::size_t admission(Code &code, const ResidentPlan &plan) {
  if (plan.loop) {
    trip_count(code, plan);
    constexpr std::array<std::uint8_t, 3> multiply{0x4d, 0x69, 0xda};
    code.raw(multiply);
    code.u32(static_cast<std::uint32_t>(plan.operations.size() + plan.backedge_instructions));
    if (plan.backedge_instructions != 0) {
      constexpr std::array<std::uint8_t, 3> final_exit{0x49, 0x81, 0xeb}; // SUB R11,imm32.
      code.raw(final_exit);
      code.u32(plan.backedge_instructions);
    }
  } else {
    constexpr std::array<std::uint8_t, 2> count{0x41, 0xbb};
    code.raw(count);
    code.u32(static_cast<std::uint32_t>(plan.operations.size()));
  }
  constexpr std::array<std::uint8_t, 5> compare{0x89, 0xd0, 0x4c, 0x39, 0xd8};
  code.raw(compare);
  constexpr std::array<std::uint8_t, 2> below{0x0f, 0x82};
  code.raw(below);
  return code.relative32();
}

std::vector<std::size_t> bounded_admission(Code &code, const ResidentPlan &plan) {
  // R10 retains the trip count from the preceding full-loop admission.
  const auto weight =
      static_cast<std::uint32_t>(plan.operations.size() + plan.backedge_instructions);
  if (std::has_single_bit(weight)) {
    constexpr std::array<std::uint8_t, 4> shift{0x89, 0xd0, 0xc1, 0xe8}; // EAX=budget/weight.
    code.raw(shift);
    code.byte(static_cast<std::uint8_t>(std::countr_zero(weight)));
  } else {
    constexpr std::array<std::uint8_t, 8> divide{0x41, 0x89, 0xd0, 0x89, 0xd0, 0x31, 0xd2, 0xb9};
    code.raw(divide); // Save budget in R8D; divide EDX:EAX by the taken body cost.
    code.u32(weight);
    constexpr std::array<std::uint8_t, 5> quotient{0xf7, 0xf1, 0x44, 0x89, 0xc2};
    code.raw(quotient); // Restore EDX before any refusal reaches the general path.
  }
  constexpr std::array<std::uint8_t, 4> empty{0x85, 0xc0, 0x0f, 0x84};
  code.raw(empty);
  std::vector<std::size_t> refused{code.relative32()};
  // This variant only takes a proper prefix. The full-loop path handles final
  // exits, whose split backedge must not retire after the terminating branch.
  constexpr std::array<std::uint8_t, 5> compare{0x44, 0x39, 0xd0, 0x0f, 0x83};
  code.raw(compare);
  refused.push_back(code.relative32());
  constexpr std::array<std::uint8_t, 6> progress{0x41, 0x89, 0xc2, 0x4d, 0x69, 0xda};
  code.raw(progress); // R10D=batch trips; R11=actual taken-instruction total.
  code.u32(weight);
  return refused;
}

std::vector<std::uint8_t> emit_variant(const ResidentPlan &plan,
                                       const std::span<const std::uint8_t> general,
                                       const LoopMode mode) {
  Code code;
  constexpr std::array<std::uint8_t, 4> endbr{0xf3, 0x0f, 0x1e, 0xfa};
  code.raw(endbr);
  const auto refused = mode == LoopMode::bounded ? bounded_admission(code, plan)
                                                 : std::vector<std::size_t>{admission(code, plan)};
  const auto outside_memory = indexed_admission(code, plan);
  preserve(code, plan, false, mode);
  constexpr std::array<std::uint8_t, 2> retired{0x41,
                                                0x53}; // PUSH R11: one total, outside the loop.
  code.raw(retired);
  if (mode == LoopMode::bounded) {
    constexpr std::array<std::uint8_t, 3> counter{0x45, 0x89, 0xd5}; // MOV R13D,R10D.
    code.raw(counter);
  }
  registers(code, plan, false);
  if (plan.base != nullptr) {
    constexpr std::array<std::uint8_t, 2> memory{0x49, 0xbf};
    code.raw(memory);
    code.u64(reinterpret_cast<std::uintptr_t>(plan.base));
  }
  for (const auto &operation : plan.operations) {
    if (operation.initial_load) { memory_operation(code, operation, plan, false); }
    if (operation.cached_operand) { initial_operand(code, operation); }
  }
  const auto loop = code.size();
  EmissionState state;
  body(code, plan, state);
  finish_branch(code, plan, loop, mode);
  deferred_flags(code, plan, state);
  if (plan.swapped_d && (plan.dirty_words & (1U << word_registers[0])) != 0) {
    swap_word(code, word_registers[0]);
  }
  registers(code, plan, true);
  if (plan.changed != 0) { publish_flags(code, plan, state); }
  constexpr std::uint8_t pop_rax = 0x58;
  code.byte(pop_rax);
  preserve(code, plan, true, mode);
  const auto &last = plan.operations.back();
  const auto location = mode == LoopMode::bounded && plan.backedge_instructions != 0
                            ? plan.backedge_exit
                            : ExitLocation{.pc = last.pc, .opcode = last.instruction.opcode};
  describe_exit(code, location);
  constexpr std::uint8_t ret = 0xc3;
  code.byte(ret);
  for (const auto exit : refused) { code.bind(exit); }
  for (const auto exit : outside_memory) { code.bind(exit); }
  code.raw(general);
  return std::move(code).take();
}
}

std::vector<std::uint8_t> emit_resident(const ResidentPlan &plan,
                                        const std::span<const std::uint8_t> general) {
  if (!plan.loop) { return emit_variant(plan, general, LoopMode::full); }
  const auto bounded = emit_variant(plan, general, LoopMode::bounded);
  return emit_variant(plan, bounded, LoopMode::full);
}
}
