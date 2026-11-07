// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/clock.hpp"
#include "jit/code.hpp"
#include "jit/emission.hpp"
#include "jit/resident.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
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
  movzx_word = 0xb7,
  simd_to = 0x7e,
  simd_from = 0x6e,
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

unsigned preserve(Code &code, const ResidentPlan &plan, const bool restore, const LoopMode mode) {
  constexpr std::array<std::uint8_t, 5> registers{3, 5, 12, 13, 15};
  const bool operand_cache = std::ranges::any_of(plan.operations, [](const auto &operation) {
    return operation.cached_operand || operation.cache_store;
  });
  const std::array needed{(plan.used_words & (1U << registers[0])) != 0,
                          (plan.used_words & (1U << registers[1])) != 0,
                          operand_cache,
                          mode == LoopMode::bounded,
                          plan.base != nullptr};
  constexpr std::uint8_t push = 0x50;
  constexpr std::uint8_t pop = 0x58;
  unsigned saved = 0;
  for (std::size_t offset = 0; offset < registers.size(); ++offset) {
    const auto index = restore ? registers.size() - 1 - offset : offset;
    if (!needed[index]) { continue; }
    rex(code, registers[index]);
    code.byte(static_cast<std::uint8_t>((restore ? pop : push) + (registers[index] & 7U)));
    ++saved;
  }
  return saved;
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
                          operation.instruction.kind == Kind::compare ||
                          (operation.instruction.kind == Kind::register_operation &&
                           operation.instruction.operation == RegisterOp::arithmetic_left);
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
  // RAM-only loops have no SIMD value-cache slots. XMM0 retains the admitted
  // indexed origin independently of the static-operand base in R15.
  constexpr std::array<std::uint8_t, 5> origin{0x66, 0x48, 0x0f, 0x7e, 0xc0}; // MOVQ RAX,XMM0.
  code.raw(origin);
  constexpr std::array<std::uint8_t, 4> load{0x0f, 0xb7, 0x1c, 0x08}; // MOVZX EBX,[RAX+RCX].
  code.raw(load);
  swap_word(code, 3);
  constexpr std::array<std::uint8_t, 3> advance{0x8d, 0x49, 0x02}; // LEA ECX,[RCX+2].
  code.raw(advance);
  if (needed_flags) { test_value(code, operation.instruction, plan); }
}

void word_value(Code &code,
                const ResidentOperation &operation,
                const unsigned reg,
                const bool read) {
  const auto slot = static_cast<unsigned>(operation.value_slot - 1U);
  constexpr unsigned low_slots = 8;
  code.byte(word_prefix);
  if (slot >= low_slots) { code.byte(rex_r); }
  code.byte(escape);
  code.byte(read ? simd_to : simd_from); // MOVD r32,XMM or MOVD XMM,r32; flags are unchanged.
  code.byte(static_cast<std::uint8_t>(register_operand | ((slot & 7U) << 3U) | reg));
}

void fill_value(Code &code, const ResidentOperation &operation, const unsigned reg) {
  if (operation.value_slot != 0 && operation.fill_value) {
    word_value(code, operation, reg, false);
  }
}

void memory_arithmetic(Code &code, const ResidentOperation &operation, const ResidentPlan &plan) {
  const auto &instruction = operation.instruction;
  const auto reg = instruction.word ? native_word(instruction) : byte_register(instruction, plan);
  if (!operation.cached_operand) {
    if (operation.value_slot != 0 && !operation.fill_value) {
      word_value(code, operation, 0, true);
    } else {
      code.byte(rex_b);
      if (instruction.word) {
        code.byte(escape);
        code.byte(movzx_word); // MOVZX EAX,word memory: no old-register dependency.
      } else {
        code.byte(mov8_from);
      }
      code.byte(r15_displacement); // Read the actual current operand into AX/AL.
      code.u32(std::bit_cast<std::uint32_t>(operation.displacement));
      if (instruction.word) { swap_word(code, 0); }
      fill_value(code, operation, 0);
    }
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

bool register_load(Code &code, const ResidentOperation &operation, const unsigned reg) {
  if (operation.value_slot != 0 && !operation.fill_value) {
    word_value(code, operation, reg, true);
    return true;
  }
  if (operation.forwarded_word != 0) {
    if (reg != operation.forwarded_word) {
      code.byte(mov_to);
      code.byte(
          static_cast<std::uint8_t>(register_operand | (operation.forwarded_word << 3U) | reg));
    }
    if (operation.forwarded_shift != 0) {
      code.byte(shift_immediate);
      code.byte(static_cast<std::uint8_t>(shift_right_eax | reg));
      code.byte(static_cast<std::uint8_t>(operation.forwarded_shift));
    }
    fill_value(code, operation, reg);
    return true;
  }
  return false;
}

void memory_opcode(Code &code, const bool word, const bool store) {
  if (word && !store) {
    code.byte(rex_b);
    code.byte(escape);
    code.byte(movzx_word); // MOVZX r32,word memory.
    return;
  }
  if (word) { code.byte(word_prefix); }
  code.byte(rex_b);
#if defined(__MOVBE__)
  if (word && store) {
    constexpr std::array<std::uint8_t, 3> movbe_store{0x0f, 0x38, 0xf1};
    code.raw(movbe_store);
    return;
  }
#endif
  constexpr std::array<std::array<std::uint8_t, 2>, 2> opcodes{{{0x8a, 0x88}, {0x8b, 0x89}}};
  code.byte(opcodes[static_cast<std::size_t>(word)][static_cast<std::size_t>(store)]);
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
  if (!store && register_load(code, operation, reg)) {
    if (needed_flags) { test_value(code, instruction, plan); }
    return;
  }
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
  memory_opcode(code, instruction.word, store);
  code.byte(static_cast<std::uint8_t>(r15_displacement | (reg << 3U)));
  code.u32(std::bit_cast<std::uint32_t>(operation.displacement));
  if (high && !store) {
    code.byte(mov8_to);
    code.byte(static_cast<std::uint8_t>(register_operand | byte_register(instruction, plan)));
  }
  if (instruction.word && !store) { swap_word(code, reg); }
  fill_value(code, operation, value_register);
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

void multiply(Code &code,
              const ResidentOperation &operation,
              EmissionState &state,
              const bool same_operand = false) {
  constexpr std::array<std::uint8_t, 3> signed_d{0x0f, 0xbf, 0xdb};
  constexpr std::array<std::uint8_t, 3> signed_y{0x0f, 0xbf, 0xd2};
  constexpr std::array<std::uint8_t, 3> unsigned_d{0x0f, 0xb7, 0xdb};
  constexpr std::array<std::uint8_t, 3> unsigned_y{0x0f, 0xb7, 0xd2};
  const bool signed_product = operation.instruction.math == MathOp::multiply_signed;
  code.raw(signed_product ? signed_d : unsigned_d);
  if (!same_operand) { code.raw(signed_product ? signed_y : unsigned_y); }
  constexpr std::array<std::uint8_t, 2> imul{0x0f, 0xaf};
  code.raw(imul);
  code.byte(static_cast<std::uint8_t>(register_operand | (3U << 3U) |
                                      (same_operand ? 3U : 2U))); // IMUL EBX,EBX or EBX,EDX.
  constexpr std::array<std::uint8_t, 5> halves{0x89, 0xda, 0xc1, 0xea, 0x10};
  code.raw(halves); // Retain the full product in EBX and high Y in DX.
  if (operation.flags == 0) { return; }
  constexpr std::array<std::uint8_t, 6> snapshot{0x85, 0xdb, 0x9f, 0x0f, 0xb6, 0xc4};
  code.raw(snapshot); // LAHF N/Z from the full product, independently of word D.
  const auto reg = snapshots[state.flags.size()];
  rex(code, reg);
  code.byte(mov_to);
  code.byte(static_cast<std::uint8_t>(register_operand | (reg & 7U)));
  if ((operation.flags & flag_c) != 0) {
    constexpr std::array<std::uint8_t, 10> carry{
        0x0f, 0xba, 0xe3, 0x0f, 0x0f, 0x92, 0xc0, 0x0f, 0xb6, 0xc0};
    code.raw(carry); // Guest C is bit 15 of the product, not x86 multiply overflow.
    rex(code, reg);
    code.byte(0x09);
    code.byte(static_cast<std::uint8_t>(register_operand | (reg & 7U)));
  }
  state.flags.push_back({.mask = operation.flags, .reg = reg});
  if (operation.branch_flags) {
    constexpr std::array<std::uint8_t, 2> test_product{0x85, 0xdb};
    code.raw(test_product); // EBX still holds the full product, not only guest word D.
  }
}

void resident_unary(Code &code, const Instruction &instruction) {
  if (instruction.operation == RegisterOp::arithmetic_left) {
    constexpr std::array<std::uint8_t, 3> shift{0x66, 0xd1, 0xe3}; // SHL BX,1.
    code.raw(shift);
    return;
  }
  constexpr std::array<std::uint8_t, 2> increment{0x66, 0xff};
  code.raw(increment);
  const bool incrementing = instruction.operation == RegisterOp::index_increment ||
                            instruction.operation == RegisterOp::increment;
  const unsigned group = incrementing ? 0U : 1U;
  code.byte(static_cast<std::uint8_t>(register_operand | (group << 3U) | native_word(instruction)));
}

bool preserves_carry(const ResidentOperation &operation) {
  const auto &instruction = operation.instruction;
  if (instruction.kind == Kind::nop || instruction.kind == Kind::branch ||
      instruction.kind == Kind::loop) {
    return true;
  }
  if (instruction.kind == Kind::register_operation &&
      (instruction.operation == RegisterOp::index_increment ||
       instruction.operation == RegisterOp::index_decrement)) {
    return true;
  }
  if (instruction.kind != Kind::store || operation.pointer == nullptr || operation.branch_flags ||
      (!operation.deferred && operation.flags != 0)) {
    return false;
  }
#if !defined(__MOVBE__)
  if (instruction.word) { return false; } // Scratch endian rotation changes host CF.
#endif
  return true;
}

const ResidentOperation *loop_carry(const ResidentPlan &plan) {
  if (!plan.loop) { return nullptr; }
  for (const auto &operation : std::views::reverse(plan.operations)) {
    const auto kind = operation.instruction.kind;
    if (!operation.deferred && operation.flags == flag_c &&
        (kind == Kind::add || kind == Kind::subtract || kind == Kind::compare)) {
      return &operation;
    }
    if (!preserves_carry(operation)) { return nullptr; }
  }
  return nullptr;
}

#if defined(__MOVBE__)
bool independent_d(const std::span<const ResidentOperation> operations) {
  for (const auto &operation : operations) {
    const auto &instruction = operation.instruction;
    if (instruction.kind == Kind::nop || instruction.kind == Kind::branch ||
        native_word(instruction) != word_registers[0]) {
      continue;
    }
    return instruction.kind == Kind::load && instruction.word && !operation.initial_load &&
           operation.forwarded_word == 0 && operation.value_slot == 0;
  }
  return false;
}

bool stored_sum(const std::span<const ResidentOperation> operations) {
  bool stored = false;
  for (const auto &operation : operations) {
    const auto &instruction = operation.instruction;
    const bool reads_d = native_word(instruction) == word_registers[0];
    if (operation.cache_store && !reads_d) { return false; }
    if (instruction.kind == Kind::nop || instruction.kind == Kind::branch || !reads_d) { continue; }
    if (instruction.kind != Kind::store || !instruction.word || operation.pointer == nullptr ||
        (!operation.deferred && operation.flags != 0) || operation.branch_flags ||
        operation.value_slot != 0) {
      return false;
    }
    stored |= operation.cache_store;
  }
  return stored;
}
#endif

const ResidentOperation *cached_sum(const ResidentPlan &plan) {
#if defined(__MOVBE__)
  if (!plan.loop) { return nullptr; }
  const auto sum = std::ranges::find_if(
      plan.operations, [](const auto &operation) { return operation.cached_operand; });
  if (sum == plan.operations.end() || sum->instruction.kind != Kind::add ||
      !sum->instruction.word || native_word(sum->instruction) != word_registers[0]) {
    return nullptr;
  }
  const auto before = static_cast<std::size_t>(sum - plan.operations.begin());
  const std::span operations(plan.operations);
  if (!independent_d(operations.first(before)) || !stored_sum(operations.subspan(before + 1))) {
    return nullptr;
  }
  return &*sum;
#else
  static_cast<void>(plan);
  return nullptr;
#endif
}

bool store_sum(Code &code, const ResidentOperation &operation, const bool sum_live) {
  if (!sum_live || operation.instruction.kind != Kind::store ||
      native_word(operation.instruction) != word_registers[0]) {
    return false;
  }
  // MOVBE word [R15+disp32],R12W retains every original guest store.
  constexpr std::array<std::uint8_t, 6> store{word_prefix, 0x45, 0x0f, 0x38, 0xf1, 0xa7};
  code.raw(store);
  code.u32(std::bit_cast<std::uint32_t>(operation.displacement));
  return true;
}

struct BodyOptions {
  const ResidentOperation *delayed_carry = nullptr;
  const ResidentOperation *sum = nullptr;
};

bool square_input(const ResidentPlan &plan, const std::size_t index) {
  if (index + 1 >= plan.operations.size()) { return false; }
  const auto &load = plan.operations[index];
  const auto &instruction = load.instruction;
  return instruction.kind == Kind::load && instruction.word &&
         instruction.target == WordTarget::y && load.forwarded_word == word_registers[0] &&
         load.forwarded_shift == 0 && load.flags == 0 && !load.branch_flags && !load.fill_value &&
         plan.operations[index + 1].instruction.kind == Kind::multiply_divide;
}

bool needs_flags(const ResidentOperation &operation) {
  return (!operation.deferred && operation.flags != 0) || operation.branch_flags;
}

void body(Code &code,
          const ResidentPlan &plan,
          EmissionState &state,
          const BodyOptions options = {}) {
  bool sum_live = false;
  for (std::size_t index = 0; index < plan.operations.size(); ++index) {
    const auto &operation = plan.operations[index];
    const auto &instruction = operation.instruction;
    if (square_input(plan, index)) {
      multiply(code, plan.operations[++index], state, true);
      continue;
    }
    if (operation.initial_load) {
      fill_value(code, operation, native_word(instruction));
      continue;
    }
    if (instruction.kind == Kind::nop || instruction.kind == Kind::branch) { continue; }
    const bool needed_flags = needs_flags(operation);
    if ((instruction.kind == Kind::compare || instruction.kind == Kind::bit_test) &&
        !needed_flags) {
      continue;
    }
    if (instruction.kind == Kind::multiply_divide) {
      multiply(code, operation, state);
      continue;
    }
    if (&operation == options.sum) {
      constexpr std::array<std::uint8_t, 4> add{word_prefix, rex_b, 0x01, 0xdc}; // ADD R12W,BX.
      code.raw(add);
      sum_live = true;
    } else if (store_sum(code, operation, sum_live)) {
      continue; // The proof admits only deferred or dead store flags and no value fills.
    } else if (instruction.kind == Kind::register_operation || instruction.kind == Kind::loop) {
      resident_unary(code, instruction);
    } else if (instruction.addressing == Addressing::indexed) {
      indexed_operation(code, operation, plan, needed_flags);
    } else if (operation.pointer != nullptr) {
      memory_operation(code, operation, plan, needed_flags);
    } else {
      immediate_operation(code, instruction, plan, needed_flags);
    }
    if (!operation.deferred && &operation != options.delayed_carry) {
      capture(code, operation, state);
    }
  }
}

void deferred_flags(Code &code,
                    const ResidentPlan &plan,
                    EmissionState &state,
                    const unsigned known = 0,
                    const ResidentOperation *const live = nullptr) {
  for (const auto &original : plan.operations) {
    if (!original.deferred || &original == live) { continue; }
    auto operation = original;
    operation.flags &= ~known;
    if (operation.flags == 0) { continue; }
    auto instruction = operation.instruction;
    if (instruction.kind == Kind::add || instruction.kind == Kind::subtract) {
      // Reconstruct the producer's input modulo its width, then recreate its flags.
      auto inverse = instruction;
      inverse.kind = instruction.kind == Kind::add ? Kind::subtract : Kind::add;
      immediate_operation(code, inverse, plan);
      immediate_operation(code, instruction, plan);
    } else if (instruction.kind == Kind::compare || instruction.kind == Kind::bit_test) {
      if (operation.pointer != nullptr) {
        auto current = operation;
        current.value_slot = 0;
        current.fill_value = false;
        current.cached_operand = false;
        memory_arithmetic(code, current, plan);
      } else {
        immediate_operation(code, instruction, plan);
      }
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

void merge_flag_group(Code &code, const FlagValue flag, const unsigned mask, const int shift) {
  copy_snapshot(code, flag);
  const auto distance = flag.boolean ? std::countr_zero(mask) : shift;
  if (!flag.boolean) {
    code.byte(and_eax);
    code.u32(distance < 0 ? mask << static_cast<unsigned>(-distance)
                          : mask >> static_cast<unsigned>(distance));
  }
  if (distance != 0) {
    code.byte(shift_immediate);
    code.byte(distance > 0 ? shift_left_eax : shift_right_eax);
    code.byte(static_cast<std::uint8_t>(distance > 0 ? distance : -distance));
  }
  constexpr std::array<std::uint8_t, 2> merge{0x09, 0xc1};
  code.raw(merge);
}

struct KnownFlags {
  unsigned mask = 0, value = 0;
};

KnownFlags branch_flags(const unsigned condition, const bool taken) {
  constexpr unsigned carry_clear = 4;
  constexpr unsigned carry_set = 5;
  constexpr unsigned not_equal = 6;
  constexpr unsigned equal = 7;
  if (condition < carry_clear || condition > equal) { return {}; }
  const auto mask = condition < not_equal ? flag_c : flag_z;
  const bool set = taken == (condition == carry_set || condition == equal);
  return {.mask = mask, .value = set ? mask : 0U};
}

KnownFlags full_loop_flags(const ResidentPlan &plan, const LoopMode mode) {
  const auto &last = plan.operations.back().instruction;
  if (mode == LoopMode::full && plan.loop && last.kind == Kind::branch) {
    return branch_flags(last.condition, false);
  }
  return {}; // DBNE tests its counter without changing the guest CCR.
}

void publish_flags(Code &code,
                   const ResidentPlan &plan,
                   const EmissionState &state,
                   const KnownFlags known = {}) {
  constexpr auto ccr = static_cast<std::uint8_t>(offsetof(Registers, ccr));
  // Guest words are already published; ECX is free and is never a flag snapshot.
  constexpr std::array<std::uint8_t, 3> load{0x0f, 0xb6, 0x4f};
  code.raw(load);
  code.byte(ccr);
  constexpr std::array<std::uint8_t, 2> retain{0x81, 0xe1};
  code.raw(retain);
  constexpr unsigned byte_mask = 0xff;
  code.u32(byte_mask & ~(plan.changed | known.mask));
  constexpr std::array<unsigned, 4> groups{flag_c, flag_v, flag_z | flag_n, flag_h};
  constexpr std::array<int, 4> byte_shifts{0, 1, -4, 1};
  constexpr std::array<int, 4> packed_shifts{-8, 1, -12, -7};
  for (auto flag : state.flags) {
    flag.mask &= ~known.mask;
    const auto &shifts = flag.packed ? packed_shifts : byte_shifts;
    for (std::size_t index = 0; index < groups.size(); ++index) {
      const auto mask = flag.mask & groups[index];
      if (mask == 0) { continue; }
      merge_flag_group(code, flag, mask, shifts[index]);
    }
  }
  if (known.value != 0) {
    constexpr std::array<std::uint8_t, 2> set{0x83, 0xc9}; // OR ECX,imm8.
    code.raw(set);
    code.byte(static_cast<std::uint8_t>(known.value));
  }
  constexpr std::array<std::uint8_t, 2> store{0x88, 0x4f};
  code.raw(store);
  code.byte(ccr);
}

void registers(Code &code, const ResidentPlan &plan, const bool store) {
  const auto active = store ? plan.dirty_words : plan.used_words;
  for (std::size_t index = 0; index < word_offsets.size(); ++index) {
    if ((active & (1U << word_registers[index])) == 0) { continue; }
    if (!store) {
      constexpr std::array<std::uint8_t, 2> load_word{0x0f, 0xb7};
      code.raw(load_word); // Clear host upper bits without reading the old register.
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

bool carry_branch(const ResidentPlan &plan) {
  const auto &last = plan.operations.back().instruction;
  constexpr unsigned clear = 4;
  constexpr unsigned set = 5;
  return !plan.loop && last.kind == Kind::branch &&
         (last.condition == clear || last.condition == set);
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
    const bool carry = carry_branch(plan);
    if (carry || !plan.host_zero) {
      constexpr auto offset = static_cast<std::uint8_t>(offsetof(Registers, ccr));
      constexpr std::array<std::uint8_t, 2> test{0xf6, 0x47};
      code.raw(test);
      code.byte(offset);
      code.byte(carry ? flag_c : flag_z);
    }
    pc(code, next);
    code.byte(escape);
    constexpr unsigned carry_set = 5;
    const bool branch_nonzero =
        carry ? condition == carry_set : (condition == not_equal) == plan.host_zero;
    code.byte(branch_nonzero ? test8 : test_word);
    const auto skip = code.relative32();
    pc(code, target);
    code.bind(skip);
  }
}

std::vector<std::size_t> indexed_window(Code &code, const Ram region) {
  std::vector<std::size_t> exits;
  constexpr std::array<std::uint8_t, 4> load{0x0f, 0xb7, 0x47, 0x04}; // Initial X.
  constexpr std::array<std::uint8_t, 2> below{0x0f, 0x82};
  code.raw(load);
  constexpr std::uint8_t subtract_eax = 0x2d;
  code.byte(subtract_eax);
  code.u32(region.first);
  code.raw(below);
  exits.push_back(code.relative32());
  constexpr std::array<std::uint8_t, 2> capacity{0x41, 0xb8};
  code.raw(capacity);
  code.u32(static_cast<std::uint32_t>(region.bytes.size()));
  constexpr std::array<std::uint8_t, 3> remaining{0x41, 0x29, 0xc0};
  code.raw(remaining);
  code.raw(below);
  exits.push_back(code.relative32());
  // R10 is the admitted trip count, independent of retirement weighting.
  constexpr std::array<std::uint8_t, 7> extent{0x47, 0x8d, 0x0c, 0x12, 0x45, 0x39, 0xc8};
  code.raw(extent);
  code.raw(below);
  exits.push_back(code.relative32());
  constexpr std::array<std::uint8_t, 2> origin{0x49, 0xb8}; // MOV R8,physical origin.
  code.raw(origin);
  code.u64(reinterpret_cast<std::uintptr_t>(region.bytes.data()));
  constexpr std::array<std::uint8_t, 3> adjust{0x49, 0x81, 0xe8}; // SUB R8,logical start.
  code.raw(adjust);
  code.u32(region.first);
  return exits;
}

std::vector<std::size_t> indexed_admission(Code &code, const ResidentPlan &plan) {
  if (plan.indexed_regions.empty()) { return {}; }
  std::vector<std::size_t> accepted;
  constexpr std::uint8_t jump = 0xe9;
  for (const auto &region : plan.indexed_regions) {
    const auto next = indexed_window(code, region);
    code.byte(jump);
    accepted.push_back(code.relative32());
    for (const auto exit : next) { code.bind(exit); }
  }
  code.byte(jump);
  const auto refused = code.relative32();
  for (const auto entry : accepted) { code.bind(entry); }
  constexpr std::array<std::uint8_t, 5> preserve_origin{0x66, 0x49, 0x0f, 0x6e, 0xc0};
  code.raw(preserve_origin); // MOVQ XMM0,R8; all architectural state is still untouched.
  return {refused};
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

std::size_t admission(Code &code, const ResidentPlan &plan, const bool terminal = false) {
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
  constexpr std::uint8_t below = 0x82;
  constexpr std::uint8_t below_equal = 0x86;
  code.byte(escape);
  code.byte(terminal ? below_equal : below);
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

unsigned base_cycles(const ResidentPlan &plan) {
  unsigned cycles = 0;
  for (const auto &operation : plan.operations) {
    cycles += instruction_timing(operation.instruction).base;
  }
  return cycles;
}

void resident_clock(Code &code,
                    const ResidentPlan &plan,
                    const LoopMode mode,
                    const bool forwarded) {
  constexpr std::array<std::uint8_t, 2> save{0x51, 0x52};
  constexpr std::array<std::uint8_t, 2> restore{0x5a, 0x59};
  if (forwarded) { code.raw(save); }
  const auto base = base_cycles(plan);
  const auto &last = plan.operations.back();
  const auto extra = instruction_timing(last.instruction).taken_extra;
  if (plan.loop) {
    const bool split = plan.backedge_instructions != 0;
    const bool full = mode == LoopMode::full;
    const auto count = static_cast<unsigned>(plan.operations.size()) + plan.backedge_instructions;
    const auto cost = base + (split ? plan.backedge_cycles : extra);
    const auto correction = split ? static_cast<int>(extra) - static_cast<int>(plan.backedge_cycles)
                                  : -static_cast<int>(extra);
    emit_loop_progress(
        code,
        {.instructions_per_iteration = count,
         .cycles_per_iteration = cost,
         .instruction_bias = split && full ? static_cast<int>(plan.backedge_instructions) : 0,
         .cycle_bias = full ? correction : 0},
        ClockContext::argument);
  } else {
    emit_progress(code, {.estimated_cycles = base}, ClockContext::argument);
    emit_control_cycles(code,
                        last.instruction,
                        static_cast<Address>(last.pc + last.instruction.length),
                        ClockContext::argument);
  }
  if (forwarded) { code.raw(restore); }
}

Instruction terminal_instruction(const ExitLocation location) {
  constexpr unsigned wait = 0x3e;
  constexpr unsigned prefix = 0x18;
  if (location.opcode == wait) { return {.kind = Kind::wait, .opcode = wait, .length = 1}; }
  if (location.opcode == prefix) { return {.kind = Kind::stop, .opcode = prefix, .length = 2}; }
  throw std::logic_error("Unexpected CPU12 resident terminal timing");
}

ClockScale clock_grant(const ResidentPlan &plan) {
  const auto &last = plan.operations.back().instruction;
  const auto extra =
      plan.backedge_instructions != 0 ? plan.backedge_cycles : instruction_timing(last).taken_extra;
  return {.instructions_per_iteration =
              static_cast<unsigned>(plan.operations.size()) + plan.backedge_instructions,
          .cycles_per_iteration = base_cycles(plan) + extra};
}

struct RegionTerminal {
  const RegionCode &machine;
  bool pad;
  LoopMode mode = LoopMode::bounded;
  bool fallthrough = false;
  std::vector<std::size_t> entries;
  unsigned iteration_cycles = 0;
};

void join_terminal(Code &code, RegionTerminal &terminal) {
  constexpr std::array<std::uint8_t, 5> empty{0x44, 0x39, 0xf0, 0x0f, 0x83};
  code.raw(empty); // Unsigned progress >= original grant leaves the terminal unexecuted.
  const auto no_budget = code.relative32();
  constexpr std::array<std::uint8_t, 4> enter{0x41, 0x89, 0xc6, 0xe9};
  code.raw(enter); // R14D holds progress across the original instruction's real ABI callbacks.
  terminal.entries.push_back(code.relative32());
  code.bind(no_budget);
}

void emit_terminal(Code &code, const ResidentPlan &combined, const RegionTerminal &terminal) {
  for (const auto entry : terminal.entries) { code.bind(entry); }
  if (terminal.entries.empty() && !terminal.fallthrough) { return; }
  constexpr std::array<std::uint8_t, 4> align{0x48, 0x83, 0xec, 0x08};
  if (terminal.pad) { code.raw(align); }
  const auto begin = code.size();
  code.raw(terminal.machine.terminal);
  const auto complete = code.size();
  const auto instruction = terminal_instruction(terminal.machine.terminal_location);
  emit_progress(
      code, {.estimated_cycles = instruction_timing(instruction).base}, ClockContext::argument);
  emit_control_cycles(code, instruction, 0, ClockContext::argument);
  constexpr std::array<std::uint8_t, 6> completed{0x44, 0x89, 0xf0, 0xff, 0xc0, 0xe9};
  code.raw(completed); // Count the terminal instruction only after a genuine complete exit.
  const auto counted = code.relative32();
  const auto failed = code.size();
  constexpr std::array<std::uint8_t, 3> progress{0x44, 0x89, 0xf0};
  code.raw(progress);
  code.bind(counted);
  describe_exit(code, terminal.machine.terminal_location, true);
  constexpr std::array<std::uint8_t, 4> release{0x48, 0x83, 0xc4, 0x08};
  if (terminal.pad) { code.raw(release); }
  constexpr std::array<std::uint8_t, 2> pop_r14{0x41, 0x5e};
  code.raw(pop_r14);
  preserve(code, combined, true, terminal.mode);
  constexpr std::uint8_t ret = 0xc3;
  code.byte(ret);
  for (const auto exit : terminal.machine.terminal_exits) {
    code.bind(begin + exit.displacement, exit.kind == NativeExit::complete ? complete : failed);
  }
}

void resident_grant(Code &code, const ResidentPlan &plan, const RegionCode *terminal) {
  auto scale = clock_grant(plan);
  if (terminal != nullptr) {
    const auto timing = instruction_timing(terminal_instruction(terminal->terminal_location));
    scale.residual_instructions = 1;
    scale.residual_cycles = timing.base + timing.taken_extra;
  }
  emit_cycle_grant(code, scale);
}

std::vector<std::uint8_t> emit_variant(const ResidentPlan &plan,
                                       const std::span<const std::uint8_t> general,
                                       const LoopMode mode,
                                       const RegionCode *terminal = nullptr) {
  Code code;
  constexpr std::array<std::uint8_t, 4> endbr{0xf3, 0x0f, 0x1e, 0xfa};
  code.raw(endbr);
  resident_grant(code, plan, terminal);
  const auto refused = mode == LoopMode::bounded
                           ? bounded_admission(code, plan)
                           : std::vector<std::size_t>{admission(code, plan, terminal != nullptr)};
  const auto outside_memory = indexed_admission(code, plan);
  const auto saved = preserve(code, plan, false, mode);
  if (terminal != nullptr) {
    constexpr std::array<std::uint8_t, 6> original{0x41, 0x56, 0x45, 0x89, 0xde, 0x56};
    code.raw(original); // Save R14/context; retain the admitted progress in R14D.
  }
  constexpr std::array<std::uint8_t, 2> retired{0x41,
                                                0x53}; // PUSH R11: one total, outside the loop.
  if (terminal == nullptr) {
    constexpr std::uint8_t context = 0x56;
    code.byte(context);
    code.raw(retired);
  }
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
  const auto *const delayed_carry = loop_carry(plan);
  const auto *const sum = cached_sum(plan);
  body(code, plan, state, {.delayed_carry = delayed_carry, .sum = sum});
  if (!carry_branch(plan)) { finish_branch(code, plan, loop, mode); }
  // Counted backedges use DEC/JNZ and preserve the final arithmetic carry.
  if (delayed_carry != nullptr) { capture(code, *delayed_carry, state); }
  if (sum != nullptr) {
    constexpr std::array<std::uint8_t, 4> restore_d{0x41, 0x0f, 0xb7, 0xdc}; // MOVZX EBX,R12W.
    code.raw(restore_d);
  }
  const auto known = full_loop_flags(plan, mode);
  deferred_flags(code, plan, state, known.mask);
  if (plan.swapped_d && (plan.dirty_words & (1U << word_registers[0])) != 0) {
    swap_word(code, word_registers[0]);
  }
  registers(code, plan, true);
  if (plan.changed != 0 || terminal != nullptr) { publish_flags(code, plan, state, known); }
  if (carry_branch(plan)) { finish_branch(code, plan, loop, mode); }
  if (terminal != nullptr) {
    constexpr std::uint8_t pop_context = 0x5e;
    code.byte(pop_context);
    constexpr std::array<std::uint8_t, 3> progress{0x44, 0x89, 0xf0};
    code.raw(progress);
    resident_clock(code, plan, mode, true);
    const RegionTerminal joined{.machine = *terminal,
                                .pad = (saved + 1U) % 2U != 0,
                                .mode = mode,
                                .fallthrough = true,
                                .entries = {}};
    emit_terminal(code, plan, joined);
  } else {
    constexpr std::uint8_t pop_rax = 0x58;
    code.byte(pop_rax);
    constexpr std::uint8_t pop_context = 0x5e;
    code.byte(pop_context);
    resident_clock(code, plan, mode, false);
    preserve(code, plan, true, mode);
    const auto &last = plan.operations.back();
    const auto location = mode == LoopMode::bounded && plan.backedge_instructions != 0
                              ? plan.backedge_exit
                              : ExitLocation{.pc = last.pc, .opcode = last.instruction.opcode};
    describe_exit(code, location);
    constexpr std::uint8_t ret = 0xc3;
    code.byte(ret);
  }
  for (const auto exit : refused) { code.bind(exit); }
  for (const auto exit : outside_memory) { code.bind(exit); }
  code.raw(general);
  return std::move(code).take();
}

struct RegionExit {
  const ResidentPlan &phase;
  EmissionState state;
  Address pc;
  std::uint32_t completed;
  unsigned cycles = 0;
  ExitLocation location;
  KnownFlags known;
  // A branch exit retains the producer's host flags; a budget subtraction does not.
  bool native_flags_live = false;
};

void region_exit(Code &code,
                 const ResidentPlan &combined,
                 const std::uint32_t reserved,
                 RegionExit exit,
                 RegionTerminal &terminal) {
  const auto *const live =
      exit.native_flags_live ? &exit.phase.operations[exit.phase.operations.size() - 2] : nullptr;
  if (live != nullptr) {
    auto producer = *live;
    producer.flags &= ~exit.known.mask;
    capture(code, producer, exit.state);
  }
  deferred_flags(code, exit.phase, exit.state, exit.known.mask, live);
  if (combined.swapped_d && (combined.dirty_words & (1U << word_registers[0])) != 0) {
    swap_word(code, word_registers[0]);
  }
  registers(code, combined, true);
  publish_flags(code, exit.phase, exit.state, exit.known);
  pc(code, exit.pc);
  constexpr std::array<std::uint8_t, 6> progress{0x44, 0x89, 0xf0, 0x44, 0x29, 0xe8};
  code.raw(progress); // EAX = original grant - remaining reserved-iteration grant.
  if (exit.completed != reserved) {
    constexpr std::uint8_t subtract_eax = 0x2d;
    code.byte(subtract_eax);
    code.u32(reserved - exit.completed);
  }
  constexpr std::uint8_t pop_context = 0x5e;
  code.byte(pop_context);
  constexpr std::array<std::uint8_t, 2> save_clock{0x51, 0x52};
  constexpr std::array<std::uint8_t, 2> restore_clock{0x5a, 0x59};
  code.raw(save_clock);
  emit_loop_progress(code,
                     {.instructions_per_iteration = reserved,
                      .cycles_per_iteration = terminal.iteration_cycles,
                      .residual_instructions = exit.completed,
                      .residual_cycles = exit.cycles},
                     ClockContext::argument);
  code.raw(restore_clock);
  if (!terminal.machine.terminal.empty() && exit.pc == terminal.machine.terminal_location.pc) {
    join_terminal(code, terminal);
  }
  constexpr std::array<std::uint8_t, 2> pop_r14{0x41, 0x5e};
  code.raw(pop_r14);
  preserve(code, combined, true, LoopMode::bounded);
  describe_exit(code, exit.location);
  constexpr std::uint8_t ret = 0xc3;
  code.byte(ret);
}

std::size_t region_branch(Code &code, const unsigned condition, const bool inverse = false) {
  constexpr std::array<std::uint8_t, 4> conditions{0x83, 0x82, 0x85, 0x84};
  constexpr unsigned first_condition = 4;
  code.byte(escape);
  code.byte(
      static_cast<std::uint8_t>(conditions.at(condition - first_condition) ^ (inverse ? 1U : 0U)));
  return code.relative32();
}

ResidentPlan combined_registers(const ResidentRegion &region) {
  ResidentPlan plan;
  plan.base = region.head.base;
  plan.changed = region.head.changed;
  plan.used_words = region.head.used_words | region.tail.used_words;
  plan.dirty_words = region.head.dirty_words | region.tail.dirty_words;
  plan.swapped_d = region.head.swapped_d;
  return plan;
}
}

std::vector<std::uint8_t> emit_resident(const ResidentPlan &plan,
                                        const std::span<const std::uint8_t> general) {
  if (!plan.loop) { return emit_variant(plan, general, LoopMode::full); }
  const auto bounded = emit_variant(plan, general, LoopMode::bounded);
  return emit_variant(plan, bounded, LoopMode::full);
}

std::vector<std::uint8_t> emit_resident(const ResidentPlan &plan, const RegionCode machine) {
  if (!plan.loop || machine.terminal.empty()) { return emit_resident(plan, machine.general); }
  const auto original = emit_resident(plan, machine.general);
  return emit_variant(plan, original, LoopMode::full, &machine);
}

std::vector<std::uint8_t> emit_resident_region(const ResidentRegion &region,
                                               const RegionCode machine) {
  const auto combined = combined_registers(region);
  const auto &head = region.head;
  const auto &tail = region.tail;
  const auto head_count = static_cast<std::uint32_t>(head.operations.size());
  const auto count = head_count + static_cast<std::uint32_t>(tail.operations.size());
  const auto head_cycles = base_cycles(head);
  const auto tail_cycles = base_cycles(tail);
  const auto head_extra = instruction_timing(head.operations.back().instruction).taken_extra;
  const auto tail_extra = instruction_timing(tail.operations.back().instruction).taken_extra;
  Code code;
  constexpr std::array<std::uint8_t, 4> endbr{0xf3, 0x0f, 0x1e, 0xfa};
  code.raw(endbr);
  ClockScale scale{.instructions_per_iteration = count,
                   .cycles_per_iteration = head_cycles + tail_cycles + tail_extra};
  if (!machine.terminal.empty()) {
    const auto timing = instruction_timing(terminal_instruction(machine.terminal_location));
    scale.residual_instructions = 1;
    scale.residual_cycles = timing.base + timing.taken_extra;
  }
  emit_cycle_grant(code, scale);
  constexpr std::array<std::uint8_t, 2> admission{0x81, 0xfa};
  code.raw(admission); // CMP EDX, complete two-phase body size.
  code.u32(count);
  constexpr std::array<std::uint8_t, 2> below{0x0f, 0x82};
  code.raw(below);
  const auto refused = code.relative32();
  const auto saved = preserve(code, combined, false, LoopMode::bounded);
  constexpr std::array<std::uint8_t, 8> grants{0x41, 0x56, 0x41, 0x89, 0xd5, 0x41, 0x89, 0xd6};
  code.raw(grants); // Preserve R14; R13D=remaining after reserving this iteration.
  constexpr std::uint8_t push_context = 0x56;
  code.byte(push_context);
  constexpr std::array<std::uint8_t, 3> reserve{0x41, 0x81, 0xed};
  code.raw(reserve);
  code.u32(count);
  registers(code, combined, false);
  if (combined.base != nullptr) {
    constexpr std::array<std::uint8_t, 2> memory{0x49, 0xbf};
    code.raw(memory);
    code.u64(reinterpret_cast<std::uintptr_t>(combined.base));
  }
  for (auto operation : head.operations) {
    if (operation.initial_load) {
      operation.value_slot = 0;
      operation.fill_value = false;
      memory_operation(code, operation, head, false);
    }
  }
  const auto loop = code.size();
  EmissionState head_state;
  body(code, head, head_state);
  const auto head_exit = region_branch(code, head.operations.back().instruction.condition);
  EmissionState tail_state;
  body(code, tail, tail_state);
  std::optional<std::size_t> tail_exit;
  if (tail.operations.back().instruction.condition != 0) {
    tail_exit = region_branch(code, tail.operations.back().instruction.condition, true);
  }
  constexpr std::array<std::uint8_t, 3> subtract{0x41, 0x81, 0xed};
  code.raw(subtract);
  code.u32(count);
  // Reserve the next complete iteration using the subtraction's unsigned carry.
  // A failed reservation wraps R13D; the exit deduction removes that reservation.
  constexpr std::array<std::uint8_t, 2> enough{0x0f, 0x83};
  code.raw(enough);
  const auto repeat = code.relative32();
  code.bind(repeat, loop);
  const auto &last_head = head.operations.back();
  const auto &last_tail = tail.operations.back();
  const auto exit_pc = static_cast<Address>(last_tail.pc + last_tail.instruction.length);
  RegionTerminal terminal{.machine = machine,
                          .pad = (saved + 1U) % 2U != 0,
                          .entries = {},
                          .iteration_cycles = head_cycles + tail_cycles + tail_extra};
  const ExitLocation tail_location{.pc = last_tail.pc, .opcode = last_tail.instruction.opcode};
  region_exit(code,
              combined,
              count,
              {.phase = tail,
               .state = tail_state,
               .pc = head.operations.front().pc,
               .completed = 0,
               .location = tail_location,
               .known = branch_flags(last_tail.instruction.condition, true)},
              terminal);
  if (tail_exit) {
    code.bind(*tail_exit);
    region_exit(code,
                combined,
                count,
                {.phase = tail,
                 .state = tail_state,
                 .pc = exit_pc,
                 .completed = count,
                 .cycles = head_cycles + tail_cycles,
                 .location = tail_location,
                 .known = branch_flags(last_tail.instruction.condition, false),
                 .native_flags_live = true},
                terminal);
  }
  code.bind(head_exit);
  region_exit(code,
              combined,
              count,
              {.phase = head,
               .state = head_state,
               .pc = exit_pc,
               .completed = head_count,
               .cycles = head_cycles + head_extra,
               .location = {.pc = last_head.pc, .opcode = last_head.instruction.opcode},
               .known = branch_flags(last_head.instruction.condition, true),
               .native_flags_live = true},
              terminal);
  emit_terminal(code, combined, terminal);
  code.bind(refused);
  code.raw(machine.general);
  return std::move(code).take();
}
}
