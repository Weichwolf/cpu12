// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "jit/arena.hpp"
#include "jit/block.hpp"
#include "jit/resident.hpp"
#include "runtime.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr Address entry = 0x0200, first = 0x4000, second = first + 4, sum_address = 0x6000;
constexpr Address last = 0xfffc;
constexpr unsigned byte_bits = 8, byte_mask = 0xff, word_mask = 0xffff;
constexpr Byte sentinel = 0xa5;
constexpr Byte retained_flags = 0xf0;
constexpr std::array<Byte, 8> left_data{
    0x7f, 0xff, 0x80, 0x01, sentinel, sentinel, sentinel, sentinel};
constexpr std::array<Byte, 8> right_data{
    0xff, 0xff, 0x12, 0x34, sentinel, sentinel, sentinel, sentinel};
constexpr std::array<Byte, 1> postincrement_x{0x31}, decrement_y{0x36}, backedge{0xf5};
constexpr std::array<Byte, 2> accumulator{sum_address >> byte_bits, sum_address &byte_mask};
constexpr auto program = isa::program(isa::opcode<isa::Opcode::ldd_indexed>(),
                                      postincrement_x,
                                      isa::opcode<isa::Opcode::addd_extended>(),
                                      accumulator,
                                      isa::opcode<isa::Opcode::std_extended>(),
                                      accumulator,
                                      isa::opcode<isa::Opcode::dbne_relative>(),
                                      decrement_y,
                                      backedge);
// A refusal must not simulate an instruction: zero retirement and no CPU/RAM writes.
constexpr std::array<Byte, 5> refused{0x31, 0xc0, 0x31, 0xd2, 0xc3}; // XOR EAX/EAX,EDX/EDX;RET.

struct Device {
  std::array<Byte, program.size()> code = program;
  std::array<Byte, 8> left = left_data;
  std::array<Byte, 8> right = right_data;
  std::array<Byte, 8> top = right_data;
  std::array<Byte, 2> output{};
  std::array<Byte, 8192> unrelated{};
  std::array<Byte, 4> known{1, 1, 1, 1};
  std::array<Ram, 6> regions;

  explicit Device(const bool tracked = false)
      : regions{Ram{.first = 0x8000, .bytes = unrelated},
                Ram{.first = last, .bytes = std::span(top).first(4), .writable = false},
                Ram{.first = second, .bytes = std::span(right).first(4), .writable = false},
                Ram{.first = entry, .bytes = code, .writable = false},
                Ram{.first = sum_address, .bytes = output},
                Ram{.first = first,
                    .bytes = std::span(left).first(4),
                    .known = tracked ? std::span(known) : std::span<Byte>{},
                    .writable = false}} {}
};

unsigned word(const std::span<const Byte> bytes, const unsigned offset) {
  return (unsigned{bytes[offset]} << byte_bits) | bytes[offset + 1];
}

struct Input {
  Address source = first, count = 2;
  unsigned trips = 2;
  bool admitted = true;
};

void check(Device &device, const void *code, const Input input) {
  constexpr Address seed = 0x8001;
  device.output = {static_cast<Byte>(seed >> byte_bits), static_cast<Byte>(seed & byte_mask)};
  const auto left = device.left;
  const auto right = device.right;
  const auto top = device.top;
  const auto known = device.known;
  const auto unrelated = device.unrelated;
  const Registers initial{
      .pc = entry, .d = 0x2468, .x = input.source, .y = input.count, .sp = 0x9000, .ccr = 0xff};
  auto wanted = initial;
  auto wanted_output = device.output;
  if (input.admitted) {
    auto base = first;
    std::span<const Byte> bytes = device.left;
    if (input.source >= last) {
      base = last;
      bytes = device.top;
    } else if (input.source >= second) {
      base = second;
      bytes = device.right;
    }
    unsigned value = seed;
    unsigned carry = 0;
    for (unsigned index = 0; index < input.trips; ++index) {
      const auto wide = value + word(bytes, input.source - base + (index * 2));
      value = wide & word_mask;
      carry = wide >> 16;
    }
    wanted.d = static_cast<Address>(value);
    wanted.x = static_cast<Address>(input.source + (input.trips * 2));
    wanted.y = static_cast<Address>(unsigned{input.count} - input.trips);
    wanted.pc = wanted.y == 0 ? entry + program.size() : entry;
    wanted.ccr = static_cast<Byte>(retained_flags | carry | (value == 0 ? 4U : 0U) |
                                   (value >= 0x8000 ? 8U : 0U));
    wanted_output = {static_cast<Byte>(value >> byte_bits), static_cast<Byte>(value & byte_mask)};
  }
  detail::NativeContext context;
  context.cpu.registers = initial;
  auto wanted_state = context.cpu;
  wanted_state.registers = wanted;
  const auto actual = detail::enter_block(code, context.cpu.registers, context, input.trips * 4);
  const auto retired = input.admitted ? input.trips * 4 : 0U;
  if (actual.retired != retired || context.cpu != wanted_state || context.fault != Fault::none ||
      device.output != wanted_output || device.left != left || device.right != right ||
      device.top != top || device.known != known || device.unrelated != unrelated ||
      device.code != program) {
    throw std::runtime_error(
        "Native indexed admission must select the actual grant or refuse unchanged");
  }
}

void windows(const bool tracked) {
  Device device(tracked);
  const auto memory = Memory::mapped(device.regions);
  const auto operations = detail::parse_block(memory, entry);
  const auto plan = detail::resident_plan(operations, memory);
  if (!plan) { throw std::runtime_error("Authored indexed loop needs a native plan"); }
  detail::ExecutableArena arena;
  const auto *code = arena.append(detail::emit_resident(*plan, refused));
  check(device, code, {.source = first, .admitted = !tracked});
  check(device, code, {.source = second});
  check(device, code, {.source = last});
  check(device, code, {.source = last + 3, .count = 1, .trips = 1, .admitted = false});
  check(device, code, {.source = first + 1, .count = 1, .trips = 1, .admitted = !tracked});
  check(device, code, {.source = first + 3, .count = 1, .trips = 1, .admitted = false});
  check(device, code, {.source = first, .count = 3, .trips = 3, .admitted = false});
  check(device, code, {.source = first, .count = 3, .trips = 1, .admitted = !tracked});
  check(device, code, {.source = first - 1, .count = 1, .trips = 1, .admitted = false});
}
}

int main() {
  try {
    windows(false);
    windows(true);
    std::cout << "Indexed grant selection, partial budgets and word-boundary refusals passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
