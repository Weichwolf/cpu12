// SPDX-License-Identifier: GPL-3.0-only
#include "jit/memory_unary.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/indexed.hpp"
#include "jit/registers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace cpu12::detail {
namespace {
constexpr auto ccr_offset = static_cast<std::uint8_t>(offsetof(Registers, ccr));
constexpr std::array<std::uint8_t, 2> operand{0x89, 0xc8};
constexpr std::array<std::uint8_t, 5> save_address{0x66, 0x44, 0x89, 0x0c, 0x24};
constexpr std::array<std::uint8_t, 5> restore_address{0x44, 0x0f, 0xb7, 0x0c, 0x24};
constexpr std::array<std::uint8_t, 5> save_context{0x48, 0x89, 0x74, 0x24, 0x08};
constexpr std::array<std::uint8_t, 5> restore_context{0x48, 0x8b, 0x74, 0x24, 0x08};
constexpr std::array<std::uint8_t, 4> save_result{0x88, 0x44, 0x24, 0x02};
constexpr std::array<std::uint8_t, 4> save_flags{0x88, 0x44, 0x24, 0x03};
constexpr std::array<std::uint8_t, 5> restore_result{0x0f, 0xb6, 0x44, 0x24, 0x02};
constexpr std::array<std::uint8_t, 5> restore_flags{0x0f, 0xb6, 0x44, 0x24, 0x03};
constexpr std::array<std::uint8_t, 3> publish_flags{0x88, 0x47, ccr_offset};

void address(Code &code, const Instruction &instruction, const Ram ram) {
  if (instruction.addressing == Addressing::indexed) {
    indexed_address(code, instruction, ram);
  } else {
    constexpr std::array<std::uint8_t, 2> immediate_r9{0x41, 0xb9};
    code.raw(immediate_r9);
    code.u32(instruction.address);
  }
}

void finish(Code &code, const Instruction &instruction) {
  if (instruction.addressing == Addressing::indexed) { indexed_commit(code, instruction); }
  code.advance(instruction.length);
  code.return_();
}

void single_access(Code &code, const Instruction &instruction, const Ram ram) {
  if (instruction.operation == RegisterOp::test) {
    data_access(code, ram, {});
    code.raw(operand);
  } else {
    // CLR is write-only. Recreate its flags after accepted access because the
    // bus call or partial-window guard may change the host's arithmetic flags.
    unary_operation(code, instruction);
    data_access(code, ram, {.write = true});
  }
  unary_operation(code, instruction);
  unary_flags(code, instruction);
  finish(code, instruction);
}

void known_ram(Code &code, const Instruction &instruction, const Ram ram) {
  native_data_access(code, ram, {});
  code.raw(operand);
  unary_operation(code, instruction);
  // Byte stores retain the native result flags; no second bounds check or call.
  native_data_access(code, ram, {.write = true});
  unary_flags(code, instruction);
  finish(code, instruction);
}

void external(Code &code, const Instruction &instruction) {
  // Only the bus path needs staged address/result/flags. Fault epilogues release
  // this host frame; accepted external effects are never rolled back or replayed.
  code.frame();
  code.raw(save_address);
  data_access(code, {}, {});
  code.raw(operand);
  unary_operation(code, instruction);
  code.raw(save_result);
  code.raw(save_context);
  unary_flags(code, instruction, false);
  code.raw(save_flags);
  code.raw(restore_context);
  code.raw(restore_address);
  code.raw(restore_result);
  data_access(code, {}, {.write = true});
  code.raw(restore_flags);
  code.raw(publish_flags);
  finish(code, instruction);
}
}

void memory_unary_instruction(Code &code, const Instruction &instruction, const Ram ram) {
  address(code, instruction, ram);
  if (instruction.operation == RegisterOp::clear || instruction.operation == RegisterOp::test) {
    single_access(code, instruction, ram);
    return;
  }
  if (!ram.bytes.empty()) {
    if (ram.first == 0 && ram.bytes.size() == address_bytes) {
      known_ram(code, instruction, ram);
      return;
    }
    const auto outside = outside_byte_ram(code, ram);
    known_ram(code, instruction, ram);
    code.bind(outside);
  }
  external(code, instruction);
}
}
