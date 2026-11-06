// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
using detail::Kind;
constexpr Address entry = 0x0200, stack = 0x9000, result_d = 0x8000, initial_word = 0x7fff;
constexpr std::array<Byte, 7> frame_head{0x5a, 0, 0x80, 0x24, 0x68, 0xab, 0xcd};
constexpr Byte sentinel = 0xa5, result_flags = 0x5a;
constexpr unsigned byte_bits = 8, frame_bytes = 9;
// LDD #$7fff; ADDD #1; NOP; STOP. The word sum sets N/V, preserving X/I.
constexpr std::array<Byte, 9> program{0xcc, 0x7f, 0xff, 0xc3, 0, 1, 0xa7, 0x18, 0x3e};
constexpr std::array<unsigned, 4> offsets{0, 3, 6, 7}, lengths{3, 3, 1, 2};
constexpr std::array kinds{Kind::load, Kind::add, Kind::nop, Kind::stop};
constexpr Registers seed{.d = 0x1357, .x = 0x2468, .y = 0xabcd, .sp = stack, .ccr = 0x50};

void require(const bool good, const char *const message) {
  if (!good) { throw std::runtime_error(message); }
}

void check(const std::span<const detail::BlockInstruction> instructions, const Address start) {
  require(instructions.size() == offsets.size(),
          "Immutable block keeps all four authored instructions");
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    const auto &actual = instructions[index];
    require(actual.pc == static_cast<Address>(start + offsets[index]) &&
                actual.instruction.length == lengths[index] &&
                actual.instruction.kind == kinds[index] &&
                actual.instruction.opcode == program[offsets[index]],
            "Immutable decode must retain original PC, length, kind and opcode");
  }
  require(instructions[0].instruction.operand == initial_word &&
              instructions[1].instruction.operand == 1,
          "Immediate word operands retain big-endian values");
}

void execute(const std::span<const Ram> code, const Address start) {
  std::array<Byte, frame_bytes> frame;
  frame.fill(sentinel);
  std::vector<Ram> regions(code.begin(), code.end());
  regions.push_back({.first = stack - frame_bytes, .bytes = frame});
  const auto memory = Memory::mapped(regions);
  check(detail::parse_block(memory, start), start);
  Core core(memory);
  auto initial = seed;
  initial.pc = start;
  core.set_registers(initial);
  const auto result = core.run(offsets.size());
  auto registers = initial;
  registers.d = result_d;
  registers.ccr = result_flags;
  registers.pc = static_cast<Address>(start + program.size());
  registers.sp = stack - frame_bytes;
  const State wanted{.registers = registers,
                     .failure = {},
                     .execution = ExecutionState::stopped,
                     .irq_delay = 0,
                     .returned_frame = false};
  const RunResult progress{.retired = offsets.size(),
                           .state = ExecutionState::stopped,
                           .pc = static_cast<Address>(start + offsets.back()),
                           .opcode = program[offsets.back()]};
  std::array<Byte, frame_bytes> expected{};
  std::ranges::copy(frame_head, expected.begin());
  expected[7] = static_cast<Byte>(registers.pc >> byte_bits);
  expected[8] = static_cast<Byte>(registers.pc);
  require(result == progress && core.state() == wanted && frame == expected,
          "Immutable-window execution preserves complete state, retirement and the original STOP "
          "frame");
}

struct IndexedShape {
  std::array<Byte, maximum_capture_bytes> bytes;
  unsigned length;
};

constexpr std::array indexed_shapes{IndexedShape{.bytes = {0xec, 0x31}, .length = 2},
                                    IndexedShape{.bytes = {0xec, 0xe0, 0x7f}, .length = 3},
                                    IndexedShape{.bytes = {0xec, 0xe1, 0x80}, .length = 3},
                                    IndexedShape{.bytes = {0xec, 0xe2, 0x12, 0x34}, .length = 4},
                                    IndexedShape{.bytes = {0xec, 0xe3, 0x12, 0x34}, .length = 4},
                                    IndexedShape{.bytes = {0xec, 0xe7}, .length = 2}};

void indexed_prefixes() {
  for (const auto &shape : indexed_shapes) {
    for (std::size_t length = 0; length <= shape.length; ++length) {
      const auto instruction = detail::decode_first(std::span(shape.bytes).first(length));
      if (length < shape.length) {
        require(!instruction, "Incomplete indexed extensions remain unaccepted");
      } else {
        require(instruction && instruction->kind == Kind::load && instruction->word &&
                    instruction->length == shape.length &&
                    instruction->addressing == detail::Addressing::indexed &&
                    instruction->indexed.postbyte == shape.bytes[1],
                "Borrowed indexed decoding retains the authored width and postbyte");
      }
    }
  }
}

void borrowed_prefixes() {
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    const auto bytes = std::span(program).subspan(offsets[index]);
    const auto instruction = detail::decode_first(bytes);
    require(instruction && instruction->kind == kinds[index] &&
                instruction->length == lengths[index],
            "Borrowed decoding consumes one exact instruction and ignores following bytes");
    for (std::size_t length = 0; length < lengths[index]; ++length) {
      require(!detail::decode_first(bytes.first(length)),
              "Incomplete borrowed instructions remain unaccepted");
    }
  }
}

void split_grants() {
  for (std::size_t split = 1; split < program.size(); ++split) {
    std::array<Byte, program.size()> left{};
    std::array<Byte, program.size()> right{};
    std::ranges::copy(std::span(program).first(split), left.begin());
    std::ranges::copy(std::span(program).subspan(split), right.begin());
    const auto before_left = left;
    const auto before_right = right;
    const std::array regions{
        Ram{.first = entry, .bytes = std::span(left).first(split), .writable = false},
        Ram{.first = static_cast<Address>(entry + split),
            .bytes = std::span(right).first(program.size() - split),
            .writable = false}};
    execute(regions, entry);
    require(left == before_left && right == before_right,
            "Split code and adjacent backing guards remain unchanged");
  }
  std::array<std::array<Byte, 2>, program.size()> bytes{};
  std::array<Ram, program.size()> regions;
  for (std::size_t index = 0; index < program.size(); ++index) {
    bytes[index] = {program[index], sentinel};
    regions[index] = {.first = static_cast<Address>(entry + index),
                      .bytes = std::span(bytes[index]).first(1),
                      .writable = false};
  }
  const auto before = bytes;
  execute(regions, entry);
  require(bytes == before, "One-byte grants never merge through backing guard bytes");
}

void wrap() {
  constexpr Address start = 0xfffc;
  std::array high{program[0], program[1], program[2], program[3]};
  std::array low{program[4], program[5], program[6], program[7], program[8]};
  const std::array regions{Ram{.first = start, .bytes = high, .writable = false},
                           Ram{.bytes = low, .writable = false}};
  execute(regions, start);
}

void aliases_and_knownness() {
  auto bytes = program;
  constexpr std::array<std::size_t, program.size()> counts{0, 0, 0, 1, 1, 1, 2, 3, 3};
  for (std::size_t index = 0; index < program.size(); ++index) {
    const std::array regions{Ram{.first = entry, .bytes = bytes, .writable = false},
                             Ram{.first = 0x8000, .bytes = std::span(bytes).subspan(index, 1)}};
    require(detail::parse_block(Memory::mapped(regions), entry).size() == counts[index],
            "Each writable physical alias stops before its actual containing instruction");
  }
  std::array<Byte, program.size()> known;
  known.fill(1);
  const auto memory =
      Memory::window({.first = entry, .bytes = bytes, .known = known, .writable = false});
  require(detail::parse_block(memory, entry).empty(),
          "Live knownness never becomes immutable code");
  require(bytes == program, "Compilation never modifies source or aliased bytes");
}

void complete_address_space() {
  std::array<Byte, address_bytes> bytes;
  constexpr Byte load_word = 0xcc;
  constexpr Address operand = 0xcccc;
  bytes.fill(load_word);
  const auto memory = Memory::window({.bytes = bytes, .writable = false});
  const auto instructions = detail::parse_block(memory, 0);
  require(instructions.size() == address_bytes,
          "Three-byte fallthrough visits every PC across three complete wraps");
  for (std::size_t index = 0; index < instructions.size(); ++index) {
    const auto &actual = instructions[index];
    require(actual.pc == static_cast<Address>(index * 3) && actual.instruction.kind == Kind::load &&
                actual.instruction.length == 3 && actual.instruction.word &&
                actual.instruction.operand == operand,
            "Complete logical blocks are never capped at one address-space traversal");
  }
}
}

int main() {
  try {
    borrowed_prefixes();
    indexed_prefixes();
    split_grants();
    wrap();
    aliases_and_knownness();
    complete_address_space();
    std::cout << "Immutable grant transitions, aliases, STOP state and 65,536-instruction "
                 "traversal passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
