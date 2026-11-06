// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "jit/arena.hpp"
#include "jit/block.hpp"
#include "jit/resident.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr Address entry = 0x0200, first = 0x4000, alias = 0x8000, far = 0x9000, stack = 0xf000;
constexpr Address final_d = 0x1217;
constexpr Byte flags = 0x50, sentinel = 0xa5;
constexpr std::size_t backing_size = 8192, sparse_offset = 4096;
constexpr unsigned operations = 8, byte_bits = 8, frame_bytes = 9;
constexpr std::array<Byte, 2> literal{0x12, 0x34}, first_operand{0x40, 0}, alias_operand{0x80, 0},
    far_operand{0x90, 0}, overlap_operand{0x40, 1};
// Aliases read the same physical bytes. An odd word store replaces the first
// word's low byte, then the independent far value contributes five once more.
constexpr auto program = isa::program(isa::opcode<isa::Opcode::ldd_immediate>(),
                                      literal,
                                      isa::opcode<isa::Opcode::std_extended>(),
                                      first_operand,
                                      isa::opcode<isa::Opcode::ldd_extended>(),
                                      far_operand,
                                      isa::opcode<isa::Opcode::addd_extended>(),
                                      alias_operand,
                                      isa::opcode<isa::Opcode::std_extended>(),
                                      overlap_operand,
                                      isa::opcode<isa::Opcode::ldd_extended>(),
                                      alias_operand,
                                      isa::opcode<isa::Opcode::addd_extended>(),
                                      far_operand,
                                      isa::opcode<isa::Opcode::std_extended>(),
                                      far_operand,
                                      isa::stop);
constexpr std::array<Byte, 5> refused{
    0x31, 0xc0, 0x31, 0xd2, 0xc3}; // Zero retirement; no state effects.
constexpr Registers initial{
    .pc = entry, .d = 0x5678, .x = 0x2468, .y = 0xabcd, .sp = stack, .ccr = flags};
constexpr std::array<Byte, 3> result_prefix{0x12, 0x12, 0x39};
constexpr std::array<Byte, 7> frame_prefix{flags, 0x17, 0x12, 0x24, 0x68, 0xab, 0xcd};

void require(const bool good, const char *const message) {
  if (!good) { throw std::runtime_error(message); }
}

void layout(const std::size_t offset) {
  alignas(backing_size) std::array<Byte, backing_size> backing;
  backing.fill(sentinel);
  backing[offset] = 0;
  backing[offset + 1] = 5;
  const auto before = backing;
  auto expected = before;
  for (std::size_t index = 0; index < result_prefix.size(); ++index) {
    expected[index] = result_prefix[index];
  }
  expected[offset] = static_cast<Byte>(final_d >> byte_bits);
  expected[offset + 1] = static_cast<Byte>(final_d);
  auto code = program;
  std::array<Byte, frame_bytes> frame;
  frame.fill(sentinel);
  const auto original_frame = frame;
  const std::array regions{Ram{.first = entry, .bytes = code, .writable = false},
                           Ram{.first = first, .bytes = std::span(backing).first(4)},
                           Ram{.first = alias, .bytes = std::span(backing).first(4)},
                           Ram{.first = far, .bytes = std::span(backing).subspan(offset, 2)},
                           Ram{.first = stack - frame_bytes, .bytes = frame}};
  const auto memory = Memory::mapped(regions);
  const auto instructions = detail::parse_block(memory, entry);
  require(instructions.size() == operations + 1,
          "Authored physical-alias block retains every instruction");
  const auto plan = detail::resident_plan(std::span(instructions).first(operations), memory);
  if (!plan) {
    throw std::runtime_error("Physical alias/overlap program must enter native resident lowering");
  }
  detail::ExecutableArena arena;
  const auto *native = arena.append(detail::emit_resident(*plan, refused));
  detail::NativeContext context;
  context.cpu.registers = initial;
  const auto retired = detail::enter_block(native, context.cpu.registers, context, operations);
  auto registers = initial;
  registers.d = final_d;
  registers.pc = entry + program.size() - isa::stop.size();
  const State running{.registers = registers,
                      .failure = {},
                      .execution = ExecutionState::running,
                      .irq_delay = 0,
                      .returned_frame = false};
  require(retired.retired == operations && context.fault == Fault::none && context.cpu == running &&
              backing == expected && frame == original_frame && code == program,
          "Native resident execution must preserve alias, odd-word overwrite, full state and all "
          "backing guards");

  backing = before;
  Core core(memory);
  core.set_registers(initial);
  const auto progress = core.run(operations + 1);
  registers.pc = entry + program.size();
  registers.sp = stack - frame_bytes;
  const State stopped{.registers = registers,
                      .failure = {},
                      .execution = ExecutionState::stopped,
                      .irq_delay = 0,
                      .returned_frame = false};
  std::array<Byte, frame_bytes> expected_frame{};
  for (std::size_t index = 0; index < frame_prefix.size(); ++index) {
    expected_frame[index] = frame_prefix[index];
  }
  expected_frame[7] = static_cast<Byte>(registers.pc >> byte_bits);
  expected_frame[8] = static_cast<Byte>(registers.pc);
  const RunResult result{.retired = operations + 1,
                         .state = ExecutionState::stopped,
                         .pc = static_cast<Address>(entry + program.size() - isa::stop.size()),
                         .opcode = isa::stop[0]};
  require(progress == result && core.state() == stopped && backing == expected &&
              frame == expected_frame && code == program,
          "Public Core must retain the same physical effects, state and original STOP frame");
}
}

int main() {
  try {
    layout(4);
    layout(sparse_offset);
    std::cout << "Compact and sparse physical metadata: native alias/overlap, all RAM and full "
                 "Core state passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
