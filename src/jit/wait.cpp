// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/indexed.hpp"
#include "jit/interrupt.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace cpu12::detail {
namespace {
constexpr auto sp = static_cast<std::uint8_t>(offsetof(Registers, sp));
constexpr auto ccr = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr auto state =
    static_cast<std::uint8_t>(offsetof(NativeContext, cpu) + offsetof(State, execution));
static_assert(offsetof(NativeContext, cpu) + offsetof(State, execution) < 128);
constexpr unsigned frame_bytes = 9;

struct Field {
  std::uint8_t position, source;
  bool word = true;
};

constexpr std::array<Field, 5> fields{
    {{.position = 2, .source = static_cast<std::uint8_t>(offsetof(Registers, pc))},
     {.position = 4, .source = static_cast<std::uint8_t>(offsetof(Registers, y))},
     {.position = 6, .source = static_cast<std::uint8_t>(offsetof(Registers, x))},
     {.position = 8, .source = static_cast<std::uint8_t>(offsetof(Registers, d))},
     {.position = 9, .source = ccr, .word = false}}};

void field_value(Code &code,
                 const Field field,
                 const std::uint8_t next_pc,
                 const bool native_store) {
  constexpr std::array<std::uint8_t, 3> load_word{0x0f, 0xb7, 0x47};
  constexpr std::array<std::uint8_t, 3> load_byte{0x0f, 0xb6, 0x47};
  code.raw(field.word ? load_word : load_byte);
  code.byte(field.source);
  if (field.position == 2 && next_pc != 0) {
    constexpr std::array<std::uint8_t, 2> advance{0x66, 0x05};
    code.raw(advance);
    code.u16(next_pc);
  }
  // Native little-endian stores need PC/Y/X reversed; the big-endian bus path
  // instead reverses D to produce its B,A frame order. CCR is a single byte.
  if (field.word && ((field.position == 8) != native_store)) {
    constexpr std::array<std::uint8_t, 4> reverse{0x66, 0xc1, 0xc0, 8};
    code.raw(reverse);
  }
}

bool direct_frame(const Ram &region) {
  return region.writable && region.known.empty() && region.bytes.size() >= frame_bytes;
}

std::optional<std::size_t> ram_frame(Code &code, const Memory &memory, const std::uint8_t next_pc) {
  const Ram *selected = nullptr;
  for (const auto &region : memory.regions()) {
    if (direct_frame(region) &&
        (selected == nullptr || region.bytes.size() > selected->bytes.size())) {
      selected = &region;
    }
  }
  if (selected == nullptr) { return std::nullopt; }
  // Prove the complete frame in the largest ordinary RAM grant. Other grants,
  // wrapped stacks and unknown/readonly storage keep the existing bus path.
  constexpr std::array<std::uint8_t, 5> stack{0x44, 0x0f, 0xb7, 0x57, sp};
  constexpr std::array<std::uint8_t, 3> lower{0x41, 0x81, 0xea};
  code.raw(stack);
  code.raw(lower);
  code.u32(selected->first + frame_bytes);
  constexpr std::array<std::uint8_t, 3> compare{0x41, 0x81, 0xfa};
  constexpr std::array<std::uint8_t, 2> above{0x0f, 0x87};
  code.raw(compare);
  code.u32(static_cast<std::uint32_t>(selected->bytes.size() - frame_bytes));
  code.raw(above);
  const auto fallback = code.relative32();
  constexpr std::array<std::uint8_t, 2> base{0x49, 0xbb};          // MOV R11,imm64.
  constexpr std::array<std::uint8_t, 3> address{0x4d, 0x01, 0xd3}; // ADD R11,R10.
  code.raw(base);
  code.u64(reinterpret_cast<std::uintptr_t>(selected->bytes.data()));
  code.raw(address);
  // Preserve ECX: ordinary interrupt entry still holds its real vector there.
  for (const auto field : fields) {
    field_value(code, field, next_pc, true);
    if (field.word) {
      constexpr std::array<std::uint8_t, 4> store{0x66, 0x41, 0x89, 0x43};
      code.raw(store);
    } else {
      constexpr std::array<std::uint8_t, 3> store{0x41, 0x88, 0x43};
      code.raw(store);
    }
    code.byte(static_cast<std::uint8_t>(frame_bytes - field.position));
  }
  constexpr std::uint8_t jump = 0xe9;
  code.byte(jump);
  const auto complete = code.relative32();
  code.bind(fallback);
  return complete;
}

void enter_wait(Code &code, const Instruction &instruction, const Memory &ram) {
  stack_context(code, ram, instruction.length);
  constexpr std::array<std::uint8_t, 5> stack{0x66, 0x83, 0x6f, sp, 9};
  code.raw(stack);
  code.advance(instruction.length);
  constexpr std::array<std::uint8_t, 3> publish{0xc6, 0x46, state};
  code.raw(publish);
  code.byte(static_cast<std::uint8_t>(instruction.kind == Kind::stop ? ExecutionState::stopped
                                                                     : ExecutionState::waiting));
  code.return_();
}
}

void stack_context(Code &code, const Memory &ram, const std::uint8_t next_pc) {
  const auto complete = ram_frame(code, ram, next_pc);
  for (const auto field : fields) {
    const auto displacement = static_cast<std::uint16_t>(-static_cast<int>(field.position));
    const Instruction address{.addressing = Addressing::indexed,
                              .indexed = {.base = IndexBase::sp, .displacement = displacement}};
    indexed_address(code, address, ram);
    field_value(code, field, next_pc, false);
    data_access(code, ram, {.word = field.word, .write = true});
  }
  if (complete) { code.bind(*complete); }
}

void wait_instruction(Code &code, const Instruction &instruction, const Memory &ram) {
  if (instruction.kind == Kind::stop) {
    constexpr std::array<std::uint8_t, 4> disabled{0xf6, 0x47, ccr, 0x80};
    constexpr std::array<std::uint8_t, 2> enabled{0x0f, 0x84};
    code.raw(disabled);
    code.raw(enabled);
    const auto enter = code.relative32();
    code.advance(instruction.length);
    code.return_();
    code.bind(enter);
  }
  enter_wait(code, instruction, ram);
}

std::vector<std::uint8_t> emit_stop_wake() {
  Code code;
  constexpr std::array<std::uint8_t, 4> entry{0xf3, 0x0f, 0x1e, 0xfa};
  constexpr std::array<std::uint8_t, 5> stack{0x66, 0x83, 0x47, sp, 9};
  constexpr std::array<std::uint8_t, 4> resume{0xc6, 0x46, state, 0};
  code.raw(entry);
  code.raw(stack);
  code.raw(resume);
  code.return_();
  return std::move(code).take();
}
}
