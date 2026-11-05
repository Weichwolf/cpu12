// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "stack_model.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

namespace {
using namespace cpu12;
using namespace cpu12::test::stack;
constexpr std::array<std::uint16_t, 6> positions{0, 1, 2, 0x7fff, 0xfffe, 0xffff};
constexpr std::array<std::uint16_t, 8> operands{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff};

struct Input {
  StackOp operation;
  Registers before;
  std::uint16_t operand;
};

void stack_case(Core &core, const std::span<Byte> ram, const Input input) {
  const auto operation = input.operation;
  const auto before = input.before;
  const auto wanted = expected(operation, before, input.operand);
  const auto address = operation.push ? wanted.sp : before.sp;
  const auto left = static_cast<std::uint16_t>(address - 1U);
  const auto right = static_cast<std::uint16_t>(static_cast<unsigned>(address) + operation.width);
  constexpr Byte left_guard = 0xa5;
  constexpr Byte right_guard = 0x5a;
  ram[left] = left_guard;
  ram[right] = right_guard;
  if (operation.width == 2) {
    word(ram, address, input.operand);
  } else {
    ram[address] = static_cast<Byte>(input.operand);
  }
  retired(core, std::array{operation.opcode}, before, wanted);
  const auto actual = operation.width == 2 ? word(ram, address) : ram[address];
  require(actual == (operation.push ? value(before, operation.target) : input.operand) &&
              ram[left] == left_guard && ram[right] == right_guard,
          "Stack access uses only its selected bytes, with independent big-endian data");
}

void inherent_matrix() {
  std::array<Byte, memory_bytes> ram{};
  Core core(Memory::unified(ram));
  const auto start = checked;
  for (const auto operation : operations) {
    for (const auto seed : seeds) {
      for (const auto position : positions) {
        for (const auto operand : operands) {
          auto before = seed;
          before.sp = position;
          const auto data =
              static_cast<std::uint16_t>(operation.width == 2 ? operand : operand & byte_mask);
          stack_case(core, ram, {.operation = operation, .before = before, .operand = data});
        }
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
              core.statistics().native_instructions == checked - start,
          "Known-RAM stack operations execute natively without ISA or bus helpers");
}

void full_word_domains() {
  std::array<Byte, memory_bytes> ram{};
  Core core(Memory::unified(ram));
  for (const auto operation : operations) {
    if (operation.width != 2) { continue; }
    for (unsigned operand = 0; operand < memory_bytes; ++operand) {
      auto before = seeds[0];
      before.sp = operation.push ? 1 : static_cast<std::uint16_t>(memory_bytes - 1);
      before.ccr = static_cast<Byte>(operand);
      const auto data = static_cast<std::uint16_t>(operand);
      if (operation.push) { assign(before, operation.target, data); }
      stack_case(core, ram, {.operation = operation, .before = before, .operand = data});
    }
  }
}

void full_ccr_domains() {
  std::array<Byte, memory_bytes> ram{};
  Core core(Memory::unified(ram));
  constexpr StackOp pulc{.opcode = 0x38, .target = Target::ccr, .width = 1, .push = false};
  for (const auto position : positions) {
    for (unsigned old = 0; old <= byte_mask; ++old) {
      auto before = seeds[0];
      before.sp = position;
      before.ccr = static_cast<Byte>(old);
      for (unsigned operand = 0; operand <= byte_mask; ++operand) {
        stack_case(
            core,
            ram,
            {.operation = pulc, .before = before, .operand = static_cast<std::uint16_t>(operand)});
      }
    }
  }
}

void full_byte_domains() {
  std::array<Byte, memory_bytes> ram{};
  Core core(Memory::unified(ram));
  for (const auto operation : operations) {
    if (operation.width != 1) { continue; }
    for (unsigned operand = 0; operand <= byte_mask; ++operand) {
      auto before = seeds[0];
      const auto data = static_cast<std::uint16_t>(operand);
      if (operation.push) { assign(before, operation.target, data); }
      stack_case(core, ram, {.operation = operation, .before = before, .operand = data});
    }
  }
}

void nested_program() {
  std::array<Byte, memory_bytes> ram{};
  Core core(Memory::unified(ram));
  constexpr std::uint16_t main_entry = 0x8000;
  constexpr std::uint16_t routine = 0x8100;
  constexpr std::uint16_t inner = 0x8110;
  constexpr std::array<Byte, 3> invoke{0x16, 0x81, 0};
  constexpr std::array<Byte, 8> body{0x3b, 0xcc, 0x12, 0x34, 7, 10, 0x3a, 0x3d};
  constexpr std::array<Byte, 4> nested{0xce, 0xab, 0xcd, 0x3d};
  for (unsigned i = 0; i < invoke.size(); ++i) { ram[main_entry + i] = invoke[i]; }
  for (unsigned i = 0; i < body.size(); ++i) { ram[routine + i] = body[i]; }
  for (unsigned i = 0; i < nested.size(); ++i) { ram[inner + i] = nested[i]; }
  constexpr Registers initial{.pc = main_entry, .d = 0xbeef, .x = 0, .y = 0, .sp = 1, .ccr = 0xe1};
  constexpr std::array<unsigned, 8> lengths{3, 1, 3, 2, 3, 1, 1, 1};
  constexpr std::array<Registers, 8> wanted{
      {{.pc = 0x8100, .d = 0xbeef, .x = 0, .y = 0, .sp = 0xffff, .ccr = 0xe1},
       {.pc = 0x8101, .d = 0xbeef, .x = 0, .y = 0, .sp = 0xfffd, .ccr = 0xe1},
       {.pc = 0x8104, .d = 0x1234, .x = 0, .y = 0, .sp = 0xfffd, .ccr = 0xe1},
       {.pc = 0x8110, .d = 0x1234, .x = 0, .y = 0, .sp = 0xfffb, .ccr = 0xe1},
       {.pc = 0x8113, .d = 0x1234, .x = 0xabcd, .y = 0, .sp = 0xfffb, .ccr = 0xe9},
       {.pc = 0x8106, .d = 0x1234, .x = 0xabcd, .y = 0, .sp = 0xfffd, .ccr = 0xe9},
       {.pc = 0x8107, .d = 0xbeef, .x = 0xabcd, .y = 0, .sp = 0xffff, .ccr = 0xe9},
       {.pc = 0x8003, .d = 0xbeef, .x = 0xabcd, .y = 0, .sp = 1, .ccr = 0xe9}}};
  core.set_registers(initial);
  for (unsigned i = 0; i < lengths.size(); ++i) {
    const auto before = core.registers();
    retired(core, std::span(ram).subspan(before.pc, lengths[i]), before, wanted[i]);
  }
  constexpr std::uint16_t outer_address = 0xffff;
  constexpr std::uint16_t outer_return = 0x8003;
  constexpr std::uint16_t saved_address = 0xfffd;
  constexpr std::uint16_t saved_d = 0xbeef;
  constexpr std::uint16_t inner_address = 0xfffb;
  constexpr std::uint16_t inner_return = 0x8106;
  require(
      word(ram, outer_address) == outer_return && word(ram, saved_address) == saved_d &&
          word(ram, inner_address) == inner_return,
      "Authored nested calls execute the expected original instructions and actual stack stores");
}

void refusals() {
  Core core;
  core.set_registers(seeds[0]);
  constexpr std::array<std::array<Byte, 4>, 4> incomplete{
      {{0x15, 0xe3, 0, 0}, {0x16, 0x12, 0, 0}, {0x17, 0x12, 0, 0}, {7, 0x12, 0, 0}}};
  for (const auto code : incomplete) {
    require(core.run_instruction(std::span(code).first(1)).fault == Fault::unsupported_instruction,
            "Incomplete calls refuse before stack or pointer accesses");
  }
  constexpr std::array<std::array<Byte, 2>, 3> padded{{{0x30, 0}, {0x38, 0}, {0x3d, 0}}};
  for (const auto code : padded) {
    require(core.run_instruction(code).fault == Fault::unsupported_instruction,
            "Padded inherent instructions cannot execute a stack access");
  }
  require(core.registers() == seeds[0] && core.statistics().compiled_entries == 0 &&
              core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Invalid captures never mutate CPU or external bus state");
}
}

int main() {
  try {
    inherent_matrix();
    full_word_domains();
    full_byte_domains();
    full_ccr_domains();
    nested_program();
    calls();
    external_bus();
    refusals();
    std::cout << "Verified " << checked
              << " native stack/call cases, CCR X restrictions, nested returns, "
                 "overlapping pointers, wrapped data and explicit external faults.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
