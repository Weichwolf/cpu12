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
#include <stdexcept>

namespace {
using namespace cpu12;
using namespace cpu12::test::resident;
constexpr std::array<Byte, 1> post_two{0x31};
constexpr std::array<Byte, 2> backedge{0x36, 0xfb};
constexpr auto program = isa::program(isa::opcode<isa::Opcode::ldd_indexed>(),
                                      post_two,
                                      isa::opcode<isa::Opcode::dbne_relative>(),
                                      backedge);
constexpr unsigned byte_mask = 255;
constexpr unsigned sign = 0x8000, nzv = 14;
constexpr Byte flags = 0xa1;

Address word(const std::array<Byte, address_bytes> &ram, const Address address) {
  return static_cast<Address>((unsigned{ram[address]} << 8U) |
                              ram[static_cast<Address>(address + 1U)]);
}

void check(Device &device, const Registers initial, const unsigned count, const Registers wanted) {
  device.core.set_registers(initial);
  const auto result = device.core.run(count);
  if (result.fault != Fault::none || device.core.registers() != wanted || result.retired != count) {
    const auto actual = device.core.registers();
    std::cerr << "source=" << initial.x << " iterations=" << initial.y
              << " fault=" << unsigned{static_cast<Byte>(result.fault)} << " d=" << actual.d << "/"
              << wanted.d << " x=" << actual.x << "/" << wanted.x << " y=" << actual.y << "/"
              << wanted.y << " ccr=" << unsigned{actual.ccr} << "/" << unsigned{wanted.ccr}
              << " pc=" << actual.pc << "/" << wanted.pc << " retired=" << result.retired << '\n';
    throw std::runtime_error("Indexed native state differs from independent expectations");
  }
}

void boundary_cases() {
  Device device(program);
  for (unsigned index = 0; index < address_bytes; ++index) {
    device.ram[index] = static_cast<Byte>(((index * 3) + 7) & byte_mask);
  }
  std::ranges::copy(program, device.ram.begin() + entry);
  const auto original = device.ram;
  constexpr std::array<Address, 6> sources{
      0, entry, entry + program.size(), 0x4000, 0xfffc, 0xfffe};
  constexpr std::array<Address, 3> counts{1, 2, 7};
  for (const auto source : sources) {
    for (const auto count : counts) {
      if (source == entry && count > 2) { continue; }
      auto initial = seed;
      initial.x = source;
      initial.y = count;
      initial.ccr = flags;
      const auto last = static_cast<Address>(unsigned{source} + ((unsigned{count} - 1U) * 2U));
      auto wanted = initial;
      wanted.d = word(original, last);
      wanted.x = static_cast<Address>(unsigned{source} + (unsigned{count} * 2U));
      wanted.y = 0;
      wanted.pc = entry + program.size();
      wanted.ccr = static_cast<Byte>((flags & ~nzv) | (wanted.d == 0 ? 4U : 0U) |
                                     (wanted.d >= sign ? 8U : 0U));
      check(device, initial, unsigned{count} * 2U, wanted);
      device.core.set_registers(initial);
      const auto partial = device.core.run((unsigned{count} * 2U) - 1);
      auto before_branch = wanted;
      before_branch.pc = entry + 2;
      before_branch.y = 1;
      require(partial.fault == Fault::none && device.core.registers() == before_branch,
              "A small indexed-loop grant must stop after the actual last load");
      const auto finish = device.core.run(1);
      require(finish.fault == Fault::none && finish.retired == 1 &&
                  device.core.registers() == wanted && device.ram == original,
              "An indexed loop resumes across address wrap without extra RAM effects");
    }
  }
}

void previous_index_flags() {
  constexpr std::array<Byte, 2> comparison{0x40, 1};
  constexpr std::array<Byte, 2> repeat{0x36, 0xf8};
  constexpr auto code = isa::program(isa::opcode<isa::Opcode::cpx_immediate>(),
                                     comparison,
                                     isa::opcode<isa::Opcode::ldd_indexed>(),
                                     post_two,
                                     isa::opcode<isa::Opcode::dbne_relative>(),
                                     repeat);
  Device device(code);
  constexpr Address loaded_word = 0x1234;
  device.ram[data] = static_cast<Byte>(loaded_word >> 8U);
  device.ram[data + 1] = static_cast<Byte>(loaded_word & byte_mask);
  auto initial = seed;
  initial.x = data;
  initial.y = 1;
  auto wanted = initial;
  wanted.x = data + 2;
  wanted.y = 0;
  wanted.d = loaded_word;
  wanted.pc = entry + code.size();
  wanted.ccr = 1;
  check(device, initial, 3, wanted);
}

void final_index_wrap() {
  constexpr std::array<Byte, 2> zero{0, 0};
  constexpr std::array<Byte, 2> destination{0x40, 0};
  constexpr std::array<Byte, 2> repeat{0x36, 0xf5};
  constexpr auto code = isa::program(isa::opcode<isa::Opcode::ldd_indexed>(),
                                     post_two,
                                     isa::opcode<isa::Opcode::cpx_immediate>(),
                                     zero,
                                     isa::opcode<isa::Opcode::stx_extended>(),
                                     destination,
                                     isa::opcode<isa::Opcode::dbne_relative>(),
                                     repeat);
  Device device(code);
  constexpr Address source = 0xfffe;
  constexpr Address loaded_word = 0x1234;
  device.ram[source] = static_cast<Byte>(loaded_word >> 8U);
  device.ram[source + 1U] = static_cast<Byte>(loaded_word & byte_mask);
  constexpr std::array<Byte, 3> conditions{0, 0xa1, 0xff};
  constexpr unsigned retained = 0xf0;
  for (const auto ccr : conditions) {
    auto initial = seed;
    initial.x = source;
    initial.y = 1;
    initial.ccr = ccr;
    auto wanted = initial;
    wanted.x = 0;
    wanted.y = 0;
    wanted.d = loaded_word;
    wanted.pc = entry + code.size();
    wanted.ccr = static_cast<Byte>((ccr & retained) | 4U);
    check(device, initial, 4, wanted);
    require(device.ram[data] == 0 && device.ram[data + 1] == 0,
            "CPX and STX must see wrapped guest X even while the host index is wider");
  }
}

void split_word_fault() {
  Device device(program);
  constexpr std::array<Address, 2> sources{entry + program.size() - 1, 0xffff};
  for (const auto source : sources) {
    auto initial = seed;
    initial.x = source;
    initial.y = 1;
    device.core.set_registers(initial);
    const auto result = device.core.run();
    require(result.fault == Fault::memory_read && result.retired == 0 && result.address == source &&
                result.pc == entry && device.core.registers() == initial,
            "Split words must retain the explicit word-owner fault without CPU publication");
  }
}

void unknown_operand() {
  Device device(program);
  std::array<Byte, address_bytes> known{};
  const auto after = static_cast<Address>(entry + program.size());
  const std::array regions{Ram{.bytes = std::span(device.ram).first(entry)},
                           Ram{.first = entry,
                               .bytes = std::span(device.ram).subspan(entry, program.size()),
                               .writable = false},
                           Ram{.first = after,
                               .bytes = std::span(device.ram).subspan(after),
                               .known = std::span(known).subspan(after)}};
  device.core.set_memory(Memory::mapped(regions));
  auto initial = seed;
  initial.x = data;
  initial.y = 2;
  device.core.set_registers(initial);
  const auto result = device.core.run();
  require(result.fault == Fault::memory_read && result.retired == 0 && result.address == data &&
              result.pc == entry && device.core.registers() == initial,
          "Unknown indexed bytes must use the original visible read fault without CPU publication");
}
}

int main() {
  try {
    boundary_cases();
    final_index_wrap();
    previous_index_flags();
    unknown_operand();
    split_word_fault();
    std::cout
        << "Resident indexed loops preserve ranges, wrap, partial grants, old flags and faults\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
