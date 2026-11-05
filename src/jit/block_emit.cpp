// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/code.hpp"
#include "jit/emission.hpp"
#include "jit/emitter.hpp"
#include "jit/resident.hpp"
#include "runtime.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace cpu12::detail {
namespace {
struct FailedExit {
  std::size_t displacement = 0;
  std::uint32_t completed = 0;
};

constexpr std::array<std::uint8_t, 4> endbr{0xf3, 0x0f, 0x1e, 0xfa};
constexpr std::array<std::uint8_t, 8> save{0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
constexpr std::array<std::uint8_t, 12> arguments{
    0x49, 0x89, 0xfc, 0x49, 0x89, 0xf5, 0x41, 0x89, 0xd7, 0x45, 0x31, 0xf6};
constexpr std::array<std::uint8_t, 6> restore_arguments{0x4c, 0x89, 0xe7, 0x4c, 0x89, 0xee};
constexpr std::array<std::uint8_t, 6> remaining{0x44, 0x89, 0xf8, 0x44, 0x29, 0xf0};
constexpr std::array<std::uint8_t, 3> add_count{0x41, 0x81, 0xc6};
constexpr std::array<std::uint8_t, 9> leave{0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0xc3};
constexpr std::array<std::uint8_t, 3> count_result{0x44, 0x89, 0xf0};
constexpr std::uint8_t jump = 0xe9, compare_eax = 0x3d;

bool loops(const std::span<const BlockInstruction> instructions) {
  const auto &last = instructions.back();
  const auto &instruction = last.instruction;
  if (instruction.kind != Kind::branch && instruction.kind != Kind::loop) { return false; }
  const auto target = static_cast<Address>(static_cast<int>(last.pc) + instruction.length +
                                           std::bit_cast<std::int16_t>(instruction.operand));
  return target == instructions.front().pc;
}

void append_instruction(Code &code,
                        const BlockInstruction &operation,
                        const Memory &memory,
                        const std::uint32_t completed,
                        std::vector<FailedExit> &faults) {
  code.raw(restore_arguments);
  const auto emitted = emit_joined(operation.instruction, memory);
  const auto begin = code.size();
  code.raw(emitted.bytes);
  for (const auto exit : emitted.exits) {
    if (exit.kind == NativeExit::complete) {
      code.bind(begin + exit.displacement);
    } else {
      faults.push_back({.displacement = begin + exit.displacement, .completed = completed});
    }
  }
}
}

void describe_exit(Code &code, const ExitLocation location, const bool boundary) {
  const auto metadata = std::uint32_t{location.pc} |
                        (std::uint32_t{location.opcode} << native_opcode_shift) |
                        (boundary ? native_boundary_mask : 0U);
  constexpr std::uint8_t descriptor = 0xba; // MOV EDX,imm32; RAX retains retirement progress.
  code.byte(descriptor);
  code.u32(metadata);
}

std::vector<std::uint8_t> emit_block(const std::span<const BlockInstruction> instructions,
                                     const Memory &memory) {
  Code code;
  code.raw(endbr);
  code.raw(save);
  code.raw(arguments);
  const auto begin = code.size();
  code.raw(remaining);
  code.byte(compare_eax);
  code.u32(static_cast<std::uint32_t>(instructions.size()));
  constexpr std::array<std::uint8_t, 2> below{0x0f, 0x82};
  code.raw(below);
  const auto insufficient = code.relative32();
  std::vector<FailedExit> faults;
  std::uint32_t completed = 0;
  for (const auto &operation : instructions) {
    append_instruction(code, operation, memory, completed, faults);
    ++completed;
  }
  code.raw(add_count);
  code.u32(completed);
  if (loops(instructions)) {
    constexpr auto pc = static_cast<std::uint8_t>(offsetof(Registers, pc));
    constexpr std::array<std::uint8_t, 5> compare_pc{0x66, 0x41, 0x81, 0x7c, 0x24};
    code.raw(compare_pc);
    code.byte(pc);
    code.u16(instructions.front().pc);
    constexpr std::array<std::uint8_t, 2> equal{0x0f, 0x84};
    code.raw(equal);
    const auto repeat = code.relative32();
    code.bind(repeat, begin);
  }
  code.bind(insufficient);
  code.raw(count_result);
  const auto &last = instructions.back();
  describe_exit(code,
                {.pc = last.pc, .opcode = last.instruction.opcode},
                last.instruction.kind == Kind::wait || last.instruction.kind == Kind::stop);
  const auto finished = code.size();
  code.raw(leave);
  for (const auto fault : faults) {
    code.bind(fault.displacement);
    if (fault.completed != 0) {
      code.raw(add_count);
      code.u32(fault.completed);
    }
    code.raw(count_result);
    const auto &operation = instructions[fault.completed];
    describe_exit(code, {.pc = operation.pc, .opcode = operation.instruction.opcode}, true);
    code.byte(jump);
    const auto done = code.relative32();
    code.bind(done, finished);
  }
  auto general = std::move(code).take();
  if (const auto plan = resident_loop_plan(instructions, memory)) {
    return emit_resident(*plan, general);
  }
  if (const auto plan = resident_plan(instructions, memory)) {
    return emit_resident(*plan, general);
  }
  return general;
}

#if defined(__x86_64__) && defined(__linux__)
extern "C" RawNativeResult
cpu12_enter_block(const void *, Registers *, NativeContext *, std::uint32_t) noexcept;
asm(".pushsection .text\n"
    ".globl cpu12_enter_block\n"
    ".hidden cpu12_enter_block\n"
    ".type cpu12_enter_block,@function\n"
    "cpu12_enter_block:\n"
    "endbr64\n"
    "mov %rdi,%rax\n"
    "mov %rsi,%rdi\n"
    "mov %rdx,%rsi\n"
    "mov %ecx,%edx\n"
    "jmp *%rax\n"
    ".size cpu12_enter_block,.-cpu12_enter_block\n"
    ".popsection\n");
#endif

NativeResult enter_block(const void *entry,
                         Registers &registers,
                         NativeContext &context,
                         const std::uint32_t budget) noexcept {
#if defined(__x86_64__) && defined(__linux__)
  const auto result = cpu12_enter_block(entry, &registers, &context, budget);
  return native_result(result);
#else
  static_cast<void>(entry);
  static_cast<void>(registers);
  static_cast<void>(context);
  static_cast<void>(budget);
  return {};
#endif
}
}
