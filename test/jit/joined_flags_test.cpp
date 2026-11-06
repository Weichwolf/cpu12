// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "observed_core.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
constexpr Address entry = 0x0200, data = 0x4000, port_address = 0x6000, stack = 0x9000;
constexpr Address word_mask = 0xffff, sign = 0x8000, offered = 0x8001;
constexpr unsigned byte_bits = 8, byte_mask = 255, flag_c = 1, flag_v = 2;
constexpr unsigned flag_z = 4, flag_n = 8, flag_h = 0x20, frame_size = 9;
constexpr std::array<Byte, 1> one_byte{1};
constexpr std::array<Byte, 2> one_word{0, 1}, maximum_positive{0x7f, 0xff};
constexpr std::array<Byte, 2> destination{data >> byte_bits, data &byte_mask};
constexpr std::array<Byte, 2> external{port_address >> byte_bits, port_address &byte_mask};
constexpr auto program = isa::program(isa::opcode<isa::Opcode::adda_immediate>(),
                                      one_byte,
                                      isa::emuls,
                                      isa::opcode<isa::Opcode::std_extended>(),
                                      destination,
                                      isa::asld,
                                      isa::opcode<isa::Opcode::ldd_immediate>(),
                                      maximum_positive,
                                      isa::opcode<isa::Opcode::addd_immediate>(),
                                      one_word,
                                      isa::opcode<isa::Opcode::ldd_extended>(),
                                      external,
                                      isa::opcode<isa::Opcode::ldd_immediate>(),
                                      one_word,
                                      isa::stop);
constexpr std::array<unsigned, 9> lengths{2, 2, 3, 1, 3, 3, 3, 3, 2};
using Image = std::array<Byte, address_bytes>;
using Trace = std::array<Registers, lengths.size() + 1>;

unsigned nz(const unsigned value, const unsigned negative) {
  return (value == 0 ? flag_z : 0U) | ((value & negative) != 0 ? flag_n : 0U);
}

Byte flags(const Byte old, const unsigned changed, const unsigned value) {
  return static_cast<Byte>((old & (byte_mask ^ changed)) | value);
}

Registers add_one_to_a(Registers state) {
  constexpr unsigned byte_sign = 0x80;
  constexpr unsigned maximum = 0x7f;
  const auto a = unsigned{state.d} >> byte_bits;
  const auto next = (a + 1U) & byte_mask;
  state.d = static_cast<Address>((next << byte_bits) | (state.d & byte_mask));
  state.ccr = flags(state.ccr,
                    flag_h | flag_n | flag_z | flag_v | flag_c,
                    nz(next, byte_sign) | (a == maximum ? flag_v : 0U) |
                        (a == byte_mask ? flag_c : 0U) | ((a & 15U) == 15 ? flag_h : 0U));
  return state;
}

Registers signed_product(Registers state) {
  const auto signed_value = [](const Address value) {
    return value < sign ? int{value} : int{value} - int{unsigned{word_mask} + 1U};
  };
  const auto product =
      static_cast<std::uint32_t>(std::int64_t{signed_value(state.d)} * signed_value(state.y));
  state.d = static_cast<Address>(product & word_mask);
  state.y = static_cast<Address>(product >> 16U);
  state.ccr = flags(state.ccr,
                    flag_n | flag_z | flag_c,
                    nz(product, 0x80000000U) | ((product & sign) != 0 ? flag_c : 0U));
  return state;
}

Registers shift_left(Registers state) {
  const auto carry = (state.d & sign) != 0 ? flag_c : 0U;
  state.d = static_cast<Address>((unsigned{state.d} * 2U) & word_mask);
  const auto negative = (state.d & sign) != 0 ? flag_c : 0U;
  state.ccr = flags(state.ccr,
                    flag_n | flag_z | flag_v | flag_c,
                    nz(state.d, sign) | carry | ((carry ^ negative) << 1U));
  return state;
}

Registers word_flags(Registers state, const Address value) {
  state.d = value;
  state.ccr = flags(state.ccr, flag_n | flag_z | flag_v, nz(value, sign));
  return state;
}

Trace predict(const Registers initial) {
  constexpr Address maximum = 0x7fff;
  Trace trace;
  trace[0] = initial;
  trace[1] = add_one_to_a(trace[0]);
  trace[2] = signed_product(trace[1]);
  trace[3] = word_flags(trace[2], trace[2].d); // STD preserves D.
  trace[4] = shift_left(trace[3]);
  trace[5] = word_flags(trace[4], maximum);
  trace[6] = word_flags(trace[5], sign); // 0x7fff + 1: N/V set, Z/C clear.
  trace[6].ccr = flags(trace[6].ccr, flag_n | flag_z | flag_v | flag_c, flag_n | flag_v);
  trace[7] = word_flags(trace[6], offered);
  trace[8] = word_flags(trace[7], 1);
  trace[9] = trace[8];
  trace[9].sp = stack - frame_size;
  for (unsigned index = 0; index < lengths.size(); ++index) {
    trace[index + 1].pc = static_cast<Address>(trace[index].pc + lengths[index]);
  }
  return trace;
}

struct Port {
  const ObservedCore *core = nullptr;
  Registers expected;
  bool accept = false, observed = true, mapped = false;
  unsigned reads = 0;

  static bool read(void *owner, const Address address, Address &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.observed =
        port.observed && address == port_address && port.core->registers() == port.expected;
    ++port.reads;
    value = offered;
    return port.accept;
  }
};

void frame(Image &ram, const Registers state) {
  const std::array<Byte, frame_size> bytes{state.ccr,
                                           static_cast<Byte>(state.d & byte_mask),
                                           static_cast<Byte>(state.d >> byte_bits),
                                           static_cast<Byte>(state.x >> byte_bits),
                                           static_cast<Byte>(state.x & byte_mask),
                                           static_cast<Byte>(state.y >> byte_bits),
                                           static_cast<Byte>(state.y & byte_mask),
                                           static_cast<Byte>(state.pc >> byte_bits),
                                           static_cast<Byte>(state.pc & byte_mask)};
  std::ranges::copy(bytes, ram.begin() + stack - frame_size);
}

void check(ObservedCore &core, Port &port, Image &ram, const Trace &trace, const unsigned grant) {
  constexpr unsigned before_port = 6;
  const bool failed = !port.accept && !port.mapped && grant > before_port;
  const auto completed = failed ? before_port : grant;
  auto expected_ram = ram;
  if (completed >= 3) {
    expected_ram[data] = static_cast<Byte>(trace[2].d >> byte_bits);
    expected_ram[data + 1] = static_cast<Byte>(trace[2].d & byte_mask);
  }
  if (completed == lengths.size()) { frame(expected_ram, trace.back()); }
  core.set_registers(trace.front());
  port.expected = trace[before_port];
  port.observed = true;
  port.reads = 0;
  const auto result = core.run(grant);
  const auto location = failed ? trace[completed].pc : trace[completed - 1].pc;
  if (result.pc != location || result.opcode != program[location - entry] ||
      (failed &&
       (result.address != port_address || result.value != offered || result.access_size != 2)) ||
      result.retired != completed || result.fault != (failed ? Fault::memory_read : Fault::none) ||
      core.registers() != trace[completed] || ram != expected_ram || !port.observed ||
      port.reads != (grant > before_port && !port.mapped ? 1U : 0U) ||
      core.state().execution !=
          (completed == lengths.size() ? ExecutionState::stopped : ExecutionState::running)) {
    throw std::runtime_error("Joined flags differ at an ISA, owner, fault or STOP boundary");
  }
}

void exercise() {
  Image ram{};
  std::array<Byte, 2> known{1, 1};
  ram[port_address] = static_cast<Byte>(offered >> byte_bits);
  ram[port_address + 1] = static_cast<Byte>(offered & byte_mask);
  std::ranges::copy(program, ram.begin() + entry);
  const auto end = static_cast<Address>(entry + program.size());
  const std::array regions{
      Ram{.bytes = std::span(ram).first(entry)},
      Ram{.first = entry,
          .bytes = std::span(ram).subspan(entry, program.size()),
          .writable = false},
      Ram{.first = end, .bytes = std::span(ram).subspan(end, port_address - end)},
      Ram{.first = port_address, .bytes = std::span(ram).subspan(port_address, 2), .known = known},
      Ram{.first = port_address + 2, .bytes = std::span(ram).subspan(port_address + 2)}};
  Port port;
  Io io{.owner = &port, .read_word = Port::read};
  ObservedCore core(Memory::mapped(regions), &io);
  port.core = &core;
  // Compile while the port bytes are known, then make them unknown without
  // rebinding or recompiling. Its future owner/fault boundary remains exact.
  port.mapped = true;
  const auto warm = predict({.pc = entry, .sp = stack});
  check(core, port, ram, warm, lengths.size());
  const auto entries = core.statistics().compiled_entries;
  port.mapped = false;
  port.accept = true;
  known.fill(0);
  check(core, port, ram, warm, lengths.size());
  if (core.statistics().compiled_entries != entries) {
    throw std::runtime_error("Live knownness must not recompile the retained block");
  }
  constexpr std::array<Address, 6> seeds{0, 0x7f00, 0x7fff, 0x8000, 0xff00, word_mask};
  constexpr std::array<Address, 3> factors{0, 1, word_mask};
  unsigned cases = 0;
  for (unsigned ccr = 0; ccr < 0x80; ++ccr) {
    for (const auto seed : seeds) {
      for (const auto factor : factors) {
        const auto trace = predict({.pc = entry,
                                    .d = seed,
                                    .x = 123,
                                    .y = factor,
                                    .sp = stack,
                                    .ccr = static_cast<Byte>(ccr)});
        for (const auto accept : {false, true}) {
          port.accept = accept;
          for (unsigned grant = 1; grant <= lengths.size(); ++grant) {
            ram[data] = ram[data + 1] = 0;
            std::fill_n(ram.begin() + stack - frame_size, frame_size, Byte{0});
            check(core, port, ram, trace, grant);
            ++cases;
          }
        }
      }
    }
  }
  std::cout << "Independent joined-flag boundaries passed: " << cases << '\n';
}
}

int main() {
  try {
    exercise();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
