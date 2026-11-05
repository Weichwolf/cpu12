// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "resident_fixture.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <span>

namespace {
using namespace cpu12;
using namespace cpu12::test::resident;
constexpr Address alias = 0x6000;
constexpr unsigned word_mask = 0xffff, byte_bits = 8, byte_mask = 255, retained_flags = 0xf0,
                   sign = 0x8000;
constexpr std::array<Byte, 2> one{0, 1}, source{0x40, 0}, loop{0x36, 0xf4};
constexpr auto load = isa::opcode<isa::Opcode::ldd_immediate>();
constexpr auto add = isa::opcode<isa::Opcode::addd_extended>();
constexpr auto store = isa::opcode<isa::Opcode::std_extended>();
constexpr auto branch = isa::opcode<isa::Opcode::dbne_relative>();

struct Input {
  Address value, count;
  unsigned destination;
  bool store_first;
  Byte ccr;
};

Address word(const std::array<Byte, address_bytes> &ram, const Address at) {
  return static_cast<Address>((unsigned{ram[at]} << byte_bits) | ram[at + 1U]);
}

void word(std::array<Byte, address_bytes> &ram, const Address at, const Address value) {
  ram[at] = static_cast<Byte>(value >> byte_bits);
  ram[at + 1U] = static_cast<Byte>(value & byte_mask);
}

void check(const Input input) {
  constexpr std::array<std::array<Byte, 2>, 3> destinations{{{0x40, 0}, {0x40, 1}, {0x60, 0}}};
  constexpr auto prototype = isa::program(load, one, add, source, store, source, branch, loop);
  auto program = prototype;
  std::ranges::copy(destinations[input.destination], program.begin() + 7);
  if (input.store_first) {
    std::ranges::swap_ranges(std::span(program).subspan(3, 3), std::span(program).subspan(6, 3));
  }
  Device device(program);
  const auto after = static_cast<Address>(entry + program.size());
  const std::array regions{
      Ram{.bytes = std::span(device.ram).first(entry)},
      Ram{.first = entry,
          .bytes = std::span(device.ram).subspan(entry, program.size()),
          .writable = false},
      Ram{.first = after, .bytes = std::span(device.ram).subspan(after, alias - after)},
      Ram{.first = alias, .bytes = std::span(device.ram).subspan(data, 2)},
      Ram{.first = alias + 2, .bytes = std::span(device.ram).subspan(alias + 2)}};
  device.core.set_memory(Memory::mapped(regions));
  word(device.ram, data, input.value);
  auto expected_ram = device.ram;
  const auto destination = static_cast<Address>(data + (input.destination == 1 ? 1 : 0));
  unsigned result = 0;
  unsigned carry = 0;
  for (unsigned iteration = 0; iteration < input.count; ++iteration) {
    if (input.store_first) { word(expected_ram, destination, 1); }
    const auto wide = unsigned{word(expected_ram, data)} + 1U;
    result = wide & word_mask;
    carry = wide > word_mask ? 1U : 0U;
    if (!input.store_first) { word(expected_ram, destination, static_cast<Address>(result)); }
  }
  auto initial = seed;
  initial.y = input.count;
  initial.ccr = input.ccr;
  auto wanted = initial;
  wanted.y = 0;
  wanted.d = static_cast<Address>(result);
  wanted.pc = entry + program.size();
  wanted.ccr = static_cast<Byte>((initial.ccr & retained_flags) | carry | (result == 0 ? 4U : 0U) |
                                 (result >= sign ? 8U : 0U));
  device.check(initial, unsigned{input.count} * 4U, wanted);
  require(device.ram == expected_ram,
          "Forwarded operands must preserve every full, partial or physically aliased store");
  const auto compiled = device.core.statistics().compiled_entries;
  device.ram = expected_ram;
  word(device.ram, data, input.value);
  device.check(initial, unsigned{input.count} * 4U, wanted);
  require(device.ram == expected_ram && device.core.statistics().compiled_entries == compiled,
          "Warm forwarding must refresh its initial RAM value without recompilation");
}
}

int main() {
  try {
    constexpr std::array<Address, 3> values{0, 0x7fff, 0xffff};
    constexpr std::array<Address, 3> counts{1, 2, 7};
    constexpr std::array<Byte, 3> conditions{0, 0x25, 0xff};
    for (const auto value : values) {
      for (const auto count : counts) {
        for (unsigned destination = 0; destination < 3; ++destination) {
          for (const bool first : {false, true}) {
            for (const auto ccr : conditions) {
              check({.value = value,
                     .count = count,
                     .destination = destination,
                     .store_first = first,
                     .ccr = ccr});
            }
          }
        }
      }
    }
    std::cout << "Resident operands preserve physical aliases, partial overlaps and store order\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
