// SPDX-License-Identifier: GPL-3.0-only
#include "jit/interrupt.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/indexed.hpp"
#include "memory_access.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace cpu12::detail {
namespace {
struct Part {
  std::uint8_t position, destination;
  bool word = true;
};

constexpr auto ccr = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr auto sp = static_cast<std::uint8_t>(offsetof(Registers, sp));
constexpr std::array<Part, 5> parts{
    {{.position = 0, .destination = ccr, .word = false},
     {.position = 1, .destination = static_cast<std::uint8_t>(offsetof(Registers, d))},
     {.position = 3, .destination = static_cast<std::uint8_t>(offsetof(Registers, x))},
     {.position = 7, .destination = static_cast<std::uint8_t>(offsetof(Registers, pc))},
     {.position = 5, .destination = static_cast<std::uint8_t>(offsetof(Registers, y))}}};

void read_part(Code &code, const Part part, const Memory &ram) {
  const Instruction address{.addressing = Addressing::indexed,
                            .indexed = {.base = IndexBase::sp, .displacement = part.position}};
  indexed_address(code, address, ram);
  data_access(code, ram, {.word = part.word});
}

void stage(Code &code, const Part part) {
  if (part.word) {
    constexpr std::uint8_t word = 0x66;
    code.byte(word);
  }
  constexpr std::array<std::uint8_t, 3> store{0x89, 0x4c, 0x24};
  constexpr std::array<std::uint8_t, 3> store_byte{0x88, 0x4c, 0x24};
  code.raw(part.word ? store : store_byte);
  code.byte(part.position);
}

void reload(Code &code, const Part part) {
  constexpr std::array<std::uint8_t, 4> word{0x0f, 0xb7, 0x4c, 0x24};
  constexpr std::array<std::uint8_t, 4> byte{0x0f, 0xb6, 0x4c, 0x24};
  code.raw(part.word ? word : byte);
  code.byte(part.position);
}

void publish(Code &code, const Part part) {
  if (!part.word) {
    // Restored X cannot rise from a previously clear mask.
    constexpr std::array<std::uint8_t, 7> mask{0x8a, 0x47, ccr, 0x0c, 0xbf, 0x20, 0xc1};
    code.raw(mask);
  } else if (part.position == 1) {
    // The architectural stack contains B:A; D's numeric value is A:B.
    constexpr std::array<std::uint8_t, 4> exchange{0x66, 0xc1, 0xc1, 0x08};
    code.raw(exchange);
  }
  if (part.word) {
    constexpr std::uint8_t word = 0x66;
    code.byte(word);
  }
  constexpr std::array<std::uint8_t, 2> store_word{0x89, 0x4f};
  constexpr std::array<std::uint8_t, 2> store_byte{0x88, 0x4f};
  code.raw(part.word ? store_word : store_byte);
  code.byte(part.destination);
}

}

void defer_interrupt(Code &code, const bool accumulator) {
  constexpr auto delay =
      static_cast<std::uint8_t>(offsetof(NativeContext, cpu) + offsetof(State, irq_delay));
  static_assert(offsetof(NativeContext, cpu) + offsetof(State, irq_delay) < 128);
  constexpr std::array<std::uint8_t, 4> old_i{0xf6, 0x47, ccr, 0x10};
  constexpr std::array<std::uint8_t, 2> zero{0x0f, 0x84};
  code.raw(old_i);
  code.raw(zero);
  const auto unchanged = code.relative32();
  std::size_t still_set = 0;
  if (accumulator) {
    constexpr std::array<std::uint8_t, 2> new_i{0xa8, 0x10};
    constexpr std::array<std::uint8_t, 2> nonzero{0x0f, 0x85};
    code.raw(new_i);
    code.raw(nonzero);
    still_set = code.relative32();
  }
  constexpr std::array<std::uint8_t, 4> defer{0xc6, 0x46, delay, 2};
  code.raw(defer);
  code.bind(unchanged);
  if (accumulator) { code.bind(still_set); }
}

std::vector<std::uint8_t> emit_interrupt(const InterruptRequest request,
                                         const Memory &ram,
                                         const InterruptFrame frame,
                                         const std::uint8_t next_pc) {
  Code code;
  constexpr std::array<std::uint8_t, 4> entry{0xf3, 0x0f, 0x1e, 0xfa};
  code.raw(entry);
  const bool external = frame == InterruptFrame::push &&
                        (plain_ram(ram).first != 0 || plain_ram(ram).bytes.size() != address_bytes);
  if (external) { code.frame(); }
  constexpr std::array<std::uint8_t, 2> vector_address{0x41, 0xb9};
  code.raw(vector_address);
  code.u32(request.vector);
  data_access(code, ram, {.word = true});
  constexpr std::array<std::uint8_t, 4> save_target{0x66, 0x89, 0x0c, 0x24};
  if (external) { code.raw(save_target); }
  if (frame == InterruptFrame::push) { stack_context(code, ram, next_pc); }
  if (external) {
    constexpr std::array<std::uint8_t, 4> target{0x0f, 0xb7, 0x0c, 0x24};
    code.raw(target);
  }
  if (frame != InterruptFrame::waiting) {
    constexpr std::array<std::uint8_t, 4> stack{0x66, 0x81, 0x47, sp};
    code.raw(stack);
    constexpr std::uint16_t frame_change = 0xfff7;
    code.u16(frame_change);
  }
  constexpr std::array<std::uint8_t, 3> masks{0x80, 0x4f, ccr};
  code.raw(masks);
  constexpr std::uint8_t irq = 0x10;
  constexpr std::uint8_t xirq = 0x50;
  code.byte(request.kind == InterruptKind::xirq ? xirq : irq);
  constexpr auto pc = static_cast<std::uint8_t>(offsetof(Registers, pc));
  constexpr std::array<std::uint8_t, 4> target_pc{0x66, 0x89, 0x4f, pc};
  code.raw(target_pc);
  constexpr auto execution =
      static_cast<std::uint8_t>(offsetof(NativeContext, cpu) + offsetof(State, execution));
  static_assert(offsetof(State, irq_delay) == offsetof(State, execution) + 1);
  static_assert(offsetof(State, returned_frame) == offsetof(State, execution) + 2);
  static_assert(offsetof(State, execution) + sizeof(std::uint32_t) <= sizeof(State));
  static_assert(static_cast<std::uint8_t>(ExecutionState::running) == 0);
  // Clear the adjacent execution/delay/frame fields and their trailing State padding.
  constexpr std::array<std::uint8_t, 7> resume{0xc7, 0x46, execution, 0, 0, 0, 0};
  code.raw(resume);
  code.return_();
  return std::move(code).take();
}

void return_interrupt(Code &code, const Memory &ram) {
  const bool external = plain_ram(ram).first != 0 || plain_ram(ram).bytes.size() != address_bytes;
  if (external) { code.frame(); }
  for (const auto part : parts) {
    read_part(code, part, ram);
    if (external) {
      stage(code, part);
    } else {
      // Full RAM cannot call the owner or fault between these publications.
      publish(code, part);
    }
  }
  if (external) {
    for (const auto part : parts) {
      reload(code, part);
      publish(code, part);
    }
  }
  constexpr std::array<std::uint8_t, 5> advance{0x66, 0x83, 0x47, sp, 9};
  code.raw(advance);
  constexpr auto control =
      static_cast<std::uint8_t>(offsetof(NativeContext, cpu) + offsetof(State, irq_delay));
  static_assert(offsetof(State, returned_frame) == offsetof(State, irq_delay) + 1);
  constexpr std::array<std::uint8_t, 6> returned{0x66, 0xc7, 0x46, control, 0, 1};
  code.raw(returned);
  code.return_();
}
}
