// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/code.hpp"
#include "jit/emission.hpp"
#include "jit/emitter.hpp"
#include "jit/interrupt.hpp"
#include "jit/resident.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace cpu12::detail {
namespace {
struct FailedExit {
  std::size_t displacement = 0;
  std::uint32_t completed = 0;
};

struct ResidentCall {
  ResidentPlan plan;
  std::size_t displacement = 0;
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

// These operations cannot observe CCR/PC, call an owner, or fail after prior
// state publication was elided. Unknown paths are observation barriers, including live
// knownness and indexed addresses. Initialized read-only data is ordinary RAM;
// writes still require the complete width in a writable grant.
bool ordinary_access(const Instruction &instruction, const Memory &memory) {
  if (instruction.addressing == Addressing::immediate) { return true; }
  if (instruction.addressing != Addressing::direct &&
      instruction.addressing != Addressing::extended) {
    return false;
  }
  const auto width = instruction.word ? 2U : 1U;
  return std::ranges::any_of(memory.regions(), [&instruction, width](const auto &region) {
    if ((instruction.kind == Kind::store && !region.writable) || !region.known.empty() ||
        instruction.address < region.first) {
      return false;
    }
    const auto offset = static_cast<std::size_t>(instruction.address - region.first);
    return offset <= region.bytes.size() && width <= region.bytes.size() - offset;
  });
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
  if ((!arithmetic && !logical) || !ordinary_access(instruction, memory)) { return std::nullopt; }
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

void publish_pc(Code &code, const Address address) {
  constexpr auto pc = static_cast<std::uint8_t>(offsetof(Registers, pc));
  // MOV word [R12+pc],imm16: publish without a load/add/store dependency chain.
  constexpr std::array<std::uint8_t, 6> store{0x66, 0x41, 0xc7, 0x44, 0x24, pc};
  code.raw(store);
  code.u16(address);
}

void append_instruction(Code &code,
                        const BlockInstruction &operation,
                        const Memory &memory,
                        const std::uint32_t completed,
                        const EmissionOptions options,
                        std::vector<FailedExit> &faults) {
  const auto emitted = emit_joined(operation.instruction, memory, options);
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

std::optional<ResidentCall> append_resident_prefix(
    Code &code, const std::span<const BlockInstruction> instructions, const Memory &memory) {
  const auto boundary = std::ranges::find_if(instructions, [&memory](const auto &operation) {
    return !flag_writes(operation.instruction, memory).has_value();
  });
  const auto count = static_cast<std::size_t>(boundary - instructions.begin());
  if (count == 0 || count == instructions.size()) { return std::nullopt; }
  auto plan = resident_plan(instructions.first(count), memory);
  if (!plan || plan->loop) { return std::nullopt; }
  code.raw(restore_arguments);
  constexpr std::uint8_t budget = 0xba; // MOV EDX,imm32: the complete prefix is admitted.
  code.byte(budget);
  code.u32(static_cast<std::uint32_t>(count));
  constexpr std::uint8_t call = 0xe8;
  code.byte(call);
  const auto displacement = code.relative32();
  return ResidentCall{.plan = std::move(*plan), .displacement = displacement};
}

std::optional<ResidentPlan> complete_plan(const std::span<const BlockInstruction> instructions,
                                          const Memory &memory) {
  if (auto plan = resident_loop_plan(instructions, memory)) { return plan; }
  return resident_plan(instructions, memory);
}

std::vector<std::uint8_t> single_entry(const BlockInstruction &operation, const Memory &memory) {
  Code code;
  code.raw(endbr);
  constexpr std::array<std::uint8_t, 4> grant{0x85, 0xd2, 0x0f, 0x84};
  code.raw(grant); // TEST EDX,EDX; a complete original instruction requires one grant.
  const auto empty = code.relative32();
  const auto emitted = emit_joined(operation.instruction, memory);
  const auto begin = code.size();
  code.raw(emitted.bytes);
  const auto complete = code.size();
  constexpr std::uint8_t result = 0xb8; // MOV EAX,imm32.
  code.byte(result);
  code.u32(1);
  const ExitLocation location{.pc = operation.pc, .opcode = operation.instruction.opcode};
  describe_exit(code,
                location,
                operation.instruction.kind == Kind::wait ||
                    operation.instruction.kind == Kind::stop);
  constexpr std::uint8_t ret = 0xc3;
  code.byte(ret);
  const auto failed = code.size();
  code.byte(result);
  code.u32(0);
  describe_exit(code, location, true);
  code.byte(ret);
  code.bind(empty, failed);
  for (const auto exit : emitted.exits) {
    code.bind(begin + exit.displacement, exit.kind == NativeExit::complete ? complete : failed);
  }
  return std::move(code).take();
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
  if (instructions.size() == 1 && !loops(instructions)) {
    return single_entry(instructions.front(), memory);
  }
  const auto resident = complete_plan(instructions, memory);
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
  const auto options = emission_options(instructions, memory);
  const auto prefix = resident ? std::nullopt : append_resident_prefix(code, instructions, memory);
  if (prefix) { completed = static_cast<std::uint32_t>(prefix->plan.operations.size()); }
  bool pending_pc = false;
  for (const auto &operation : instructions.subspan(completed)) {
    const auto policy = options[completed];
    if (pending_pc && policy.advance_pc) { publish_pc(code, operation.pc); }
    // Proven pure operations preserve RDI and do not need the context in RSI.
    if (policy.advance_pc || !pending_pc) { code.raw(restore_arguments); }
    append_instruction(code, operation, memory, completed, policy, faults);
    pending_pc = !policy.advance_pc;
    ++completed;
  }
  if (pending_pc) {
    const auto &last = instructions.back();
    publish_pc(code, static_cast<Address>(last.pc + last.instruction.length));
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
  if (prefix) {
    code.bind(prefix->displacement);
    code.raw(emit_resident(prefix->plan, {}));
  }
  auto general = std::move(code).take();
  if (resident) { general = emit_resident(*resident, general); }
  if (const auto region = resident_region_plan(instructions, memory)) {
    const auto &tail = region->tail.operations.back();
    const auto next = static_cast<Address>(tail.pc + tail.instruction.length);
    const auto following = parse_block(memory, next);
    Emission terminal;
    ExitLocation terminal_location;
    if (following.size() == 1 && (following[0].instruction.kind == Kind::wait ||
                                  following[0].instruction.kind == Kind::stop)) {
      terminal = emit_resident_wait(
          following[0].instruction,
          memory,
          // Changed words have been published in canonical order. An unchanged,
          // byte-only D may still have A/B swapped in BX; read its original state.
          {.pc = following[0].pc, .words = region->head.dirty_words | region->tail.dirty_words});
      terminal_location = {.pc = following[0].pc, .opcode = following[0].instruction.opcode};
    }
    return emit_resident_region(*region,
                                {.general = general,
                                 .terminal = terminal.bytes,
                                 .terminal_exits = terminal.exits,
                                 .terminal_location = terminal_location});
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
