// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr Address final_pc = 2, product = 0x8000, negative_one = 0xffff;
constexpr Byte product_flags = 3;

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Fetch {
  const Core *core = nullptr;
  Registers expected;
  unsigned calls = 0;
  bool observed = true;

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    auto &fetch = *static_cast<Fetch *>(owner);
    ++fetch.calls;
    fetch.observed =
        fetch.observed && address == final_pc && fetch.core->registers() == fetch.expected;
    value = isa::nop[0];
    return false;
  }
};

void fallthrough() {
  constexpr Address entry = 0xfffe;
  auto high = isa::emuls;
  auto low = isa::program(isa::nop, isa::nop);
  const std::array regions{Ram{.bytes = low, .writable = false},
                           Ram{.first = entry, .bytes = high, .writable = false}};
  Fetch fetch;
  Io io{.owner = &fetch, .fetch = Fetch::read};
  Core core(Memory::mapped(regions), &io);
  fetch.core = &core;
  constexpr Registers initial{
      .pc = entry, .d = product, .x = 123, .y = negative_one, .sp = 0x9000, .ccr = 2};
  auto expected = initial;
  expected.y = 0; // (-32768) * (-1) = 32768: N/Z clear, C set, V preserved.
  expected.ccr = product_flags;
  for (unsigned repetition = 0; repetition < 2; ++repetition) {
    for (unsigned grant = 1; grant <= 3; ++grant) {
      core.set_registers(initial);
      expected.pc = static_cast<Address>(grant - 1U);
      const auto result = core.run(grant);
      const auto location = grant == 1 ? entry : static_cast<Address>(grant - 2U);
      require(result.retired == grant && result.fault == Fault::none && result.pc == location &&
                  result.opcode == (grant == 1 ? high[0] : low[0]) &&
                  core.registers() == expected && fetch.calls == repetition,
              "Pure ROM fallthrough must publish exact wrapped PC at every grant");
    }
    core.set_registers(initial);
    expected.pc = final_pc;
    fetch.expected = expected;
    const auto result = core.run(4);
    require(result.retired == 3 && result.fault == Fault::instruction_fetch &&
                result.pc == final_pc && result.address == final_pc &&
                core.registers() == expected && fetch.observed && fetch.calls == repetition + 1,
            "A fetch fault after a pure block must observe its exact wrapped final state");
    const auto failed = core.state();
    require(core.run(0).retired == 0 && core.run(4).retired == 0 && core.state() == failed &&
                fetch.calls == repetition + 1,
            "The PC publication and refused fetch cannot replay after a latched fault");
  }
  require(high == isa::emuls && low == isa::program(isa::nop, isa::nop),
          "PC coalescing must preserve both immutable ROM fragments");
}

void relative() {
  constexpr Address entry = 0xfffb;
  constexpr Address stack = 0x9000;
  constexpr Address lea_pc = 0xfffe;
  constexpr unsigned frame_size = 9;
  // Signed five-bit displacement zero, PC base. LEAX uses the PC after its operand.
  constexpr std::array<Byte, 1> pc_relative_zero{0xc0};
  constexpr auto high = isa::program(
      isa::emuls, isa::nop, isa::opcode<isa::Opcode::leax_indexed>(), pc_relative_zero);
  std::array<Byte, address_bytes> ram{};
  std::ranges::copy(high, ram.begin() + entry);
  std::ranges::copy(isa::stop, ram.begin());
  const auto before = ram;
  auto complete = before;
  constexpr std::array<Byte, frame_size> frame{product_flags, 0, 0x80, 0, 0, 0, 0, 0, final_pc};
  std::ranges::copy(frame, complete.begin() + stack - frame_size);
  const std::array regions{
      Ram{.bytes = std::span(ram).first(isa::stop.size()), .writable = false},
      Ram{.first = isa::stop.size(),
          .bytes = std::span(ram).subspan(isa::stop.size(), entry - isa::stop.size())},
      Ram{.first = entry, .bytes = std::span(ram).subspan(entry), .writable = false}};
  Core core(Memory::mapped(regions));
  constexpr Registers initial{
      .pc = entry, .d = product, .x = 123, .y = negative_one, .sp = stack, .ccr = 2};
  for (unsigned repetition = 0; repetition < 2; ++repetition) {
    for (unsigned grant = 1; grant <= 5; ++grant) {
      ram = before;
      core.set_registers(initial);
      auto expected = initial;
      expected.y = 0;
      expected.ccr = product_flags;
      constexpr std::array<Address, 4> next{0xfffd, lea_pc, 0, final_pc};
      // The fourth instruction is STOP; a fifth grant must remain asleep.
      const bool stopped = grant >= 4;
      expected.pc = next[std::min(grant, 4U) - 1U];
      if (grant >= 3) { expected.x = 0; }
      if (stopped) { expected.sp = stack - frame_size; }
      const auto result = core.run(grant);
      const auto executed = std::min(grant, 4U);
      constexpr std::array<Address, 4> locations{entry, entry + 2, lea_pc, 0};
      require(result.retired == executed && result.fault == Fault::none &&
                  result.pc == locations[executed - 1U] && result.opcode == before[result.pc] &&
                  core.registers() == expected &&
                  core.state().execution ==
                      (stopped ? ExecutionState::stopped : ExecutionState::running) &&
                  ram == (stopped ? complete : before),
              "A PC-relative instruction and wrapped STOP frame must see the published PC");
    }
  }
}
}

int main() {
  try {
    fallthrough();
    relative();
    std::cout << "Joined PC fallthrough, wrap, fetch refusal and PC-relative STOP passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
