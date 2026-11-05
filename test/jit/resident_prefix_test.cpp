// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr Address entry = 0x0200, data = 0x4000, alias = 0x5000, external = 0x6000;
constexpr unsigned byte_bits = 8, byte_mask = 255, word_mask = 65535, sign = 32768;
constexpr Address offered = 0x8123, replacement_d = 0x89ab, replacement_y = 0x1357;
constexpr Address seed_d = 0xa55a, seed_x = 0x3456, stack = 0x9000, byte_word = 0x1234;
constexpr Address byte_seed_x = 123, byte_seed_y = 456, upper_byte_mask = 0xff00;
constexpr Byte sentinel = 0xa5, retained_flags = 0xf1, multiply_flags = 0xf2;
constexpr Byte replacement_a = 0xaa, byte_a = 0x34, byte_b = 0x12;
constexpr std::uint64_t product_mask = 0xffffffff, product_sign = 0x80000000;
using Bytes = std::array<Byte, 8>;
enum class Form : Byte { plain, d, y, byte, word, stale_y, partial_d, new_word };
unsigned checked = 0;

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

consteval auto address(const Address value) {
  return std::array<Byte, 2>{static_cast<Byte>(value >> byte_bits),
                             static_cast<Byte>(value & byte_mask)};
}

template <Form form> consteval auto modifier() {
  if constexpr (form == Form::d) {
    return isa::program(isa::opcode<isa::Opcode::ldd_immediate>(), address(replacement_d));
  } else if constexpr (form == Form::y) {
    return isa::program(isa::opcode<isa::Opcode::ldy_immediate>(), address(replacement_y));
  } else if constexpr (form == Form::byte) {
    return isa::program(isa::opcode<isa::Opcode::staa_extended>(), address(data + 1));
  } else if constexpr (form == Form::word) {
    return isa::program(isa::opcode<isa::Opcode::sty_extended>(), address(data + 1));
  } else if constexpr (form == Form::stale_y || form == Form::new_word) {
    constexpr auto store =
        form == Form::stale_y ? isa::Opcode::sty_extended : isa::Opcode::std_extended;
    return isa::program(isa::opcode<isa::Opcode::ldd_immediate>(),
                        address(replacement_d),
                        isa::opcode<store>(),
                        address(data + 1));
  } else if constexpr (form == Form::partial_d) {
    return isa::program(isa::opcode<isa::Opcode::ldaa_immediate>(),
                        std::array<Byte, 1>{replacement_a});
  } else {
    return std::array<Byte, 0>{};
  }
}

template <Form form> consteval auto program() {
  return isa::program(isa::emuls,
                      isa::opcode<isa::Opcode::sty_extended>(),
                      address(data),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(data + 2),
                      modifier<form>(),
                      isa::opcode<isa::Opcode::ldd_extended>(),
                      address(alias + 1),
                      isa::opcode<isa::Opcode::ldd_extended>(),
                      address(external));
}

Byte flags(const Byte previous, const Address value, const bool word = true) {
  return static_cast<Byte>((previous & retained_flags) | (value == 0 ? 4U : 0U) |
                           (value >= (word ? sign : 128U) ? 8U : 0U));
}

void word(Bytes &bytes, const unsigned offset, const Address value) {
  bytes.at(offset) = static_cast<Byte>(value >> byte_bits);
  bytes.at(offset + 1) = static_cast<Byte>(value & byte_mask);
}

struct Trace {
  std::array<Registers, 8> registers{};
  std::array<Bytes, 8> memory{};
  unsigned size = 0;

  void append(Registers state, const Bytes bytes, const unsigned length) {
    state.pc = static_cast<Address>(state.pc + length);
    registers.at(++size) = state;
    memory.at(size) = bytes;
  }

  void store(const unsigned offset, const Address value, const bool wide = true) {
    auto bytes = memory.at(size);
    if (wide) {
      word(bytes, offset, value);
    } else {
      bytes.at(offset) = static_cast<Byte>(value);
    }
    auto state = registers.at(size);
    state.ccr = flags(state.ccr, value, wide);
    append(state, bytes, 3);
  }
};

std::int64_t signed_word(const Address value) {
  return value < sign ? std::int64_t{value} : std::int64_t{value} - (word_mask + 1U);
}

Trace predict(const Registers initial, const Form form) {
  Trace trace;
  trace.registers[0] = initial;
  trace.memory[0].fill(sentinel);
  const auto product =
      static_cast<std::uint64_t>(signed_word(initial.d) * signed_word(initial.y)) & product_mask;
  auto state = initial;
  state.d = static_cast<Address>(product & word_mask);
  state.y = static_cast<Address>(product >> 16U);
  state.ccr =
      static_cast<Byte>((initial.ccr & multiply_flags) | (product >= product_sign ? 8U : 0U) |
                        (product == 0 ? 4U : 0U) | ((product >> 15U) & 1U));
  trace.append(state, trace.memory[0], isa::emuls.size());
  trace.store(0, state.y);
  trace.store(2, state.d);
  state = trace.registers.at(trace.size);
  if (form == Form::d || form == Form::stale_y || form == Form::new_word) {
    state.d = replacement_d;
    state.ccr = flags(state.ccr, state.d);
    trace.append(state, trace.memory.at(trace.size), 3);
  } else if (form == Form::y) {
    state.y = replacement_y;
    state.ccr = flags(state.ccr, state.y);
    trace.append(state, trace.memory.at(trace.size), 3);
  } else if (form == Form::partial_d) {
    state.d = static_cast<Address>((unsigned{replacement_a} << byte_bits) | (state.d & byte_mask));
    state.ccr = flags(state.ccr, replacement_a, false);
    trace.append(state, trace.memory.at(trace.size), 2);
  }
  state = trace.registers.at(trace.size);
  if (form == Form::byte) { trace.store(1, static_cast<Address>(state.d >> byte_bits), false); }
  if (form == Form::word || form == Form::stale_y) { trace.store(1, state.y); }
  if (form == Form::new_word) { trace.store(1, state.d); }
  state = trace.registers.at(trace.size);
  const auto bytes = trace.memory.at(trace.size);
  state.d = static_cast<Address>((unsigned{bytes[1]} << byte_bits) | bytes[2]);
  state.ccr = flags(state.ccr, state.d);
  trace.append(state, bytes, 3);
  return trace;
}

struct Port {
  Core *core = nullptr;
  Bytes *bytes = nullptr;
  Registers expected{};
  Bytes expected_bytes{};
  unsigned reads = 0;
  bool accept = false, observed = true;

  static bool read(void *owner, const Address at, Address &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    ++port.reads;
    port.observed = port.observed && at == external && port.core->registers() == port.expected &&
                    *port.bytes == port.expected_bytes;
    value = offered;
    return port.accept;
  }
};

struct Device {
  Bytes bytes{};
  Port port{.bytes = &bytes};
  Io io{.owner = &port, .read_word = Port::read};
  std::array<Ram, 3> regions;
  Core core;

  explicit Device(const std::span<Byte> code)
      : regions{Ram{.first = entry, .bytes = code, .writable = false},
                Ram{.first = data, .bytes = bytes},
                Ram{.first = alias, .bytes = bytes}},
        core(Memory::mapped(regions), &io) {
    port.core = &core;
  }

  void check(const Trace &trace,
             const std::span<const Byte> code,
             const unsigned grant,
             const bool accept) {
    bytes = trace.memory[0];
    const auto before = grant <= trace.size ? grant : trace.size;
    port.expected = trace.registers.at(trace.size);
    port.expected_bytes = trace.memory.at(trace.size);
    port.reads = 0;
    port.observed = true;
    port.accept = accept;
    core.set_registers(trace.registers[0]);
    const auto actual = core.run(grant);
    auto expected = trace.registers.at(before);
    const bool owner = grant > trace.size;
    const bool refused = owner && !accept;
    const auto pc = owner ? trace.registers.at(trace.size).pc : trace.registers.at(grant - 1).pc;
    if (owner && accept) {
      expected.d = offered;
      expected.ccr = flags(expected.ccr, offered);
      expected.pc += 3;
    }
    const bool matched = actual.retired == grant - (refused ? 1U : 0U) && actual.pc == pc &&
                         actual.opcode == code[pc - entry] &&
                         actual.state == ExecutionState::running &&
                         actual.fault == (refused ? Fault::memory_read : Fault::none) &&
                         core.registers() == expected && bytes == trace.memory.at(before) &&
                         port.reads == (owner ? 1U : 0U) && port.observed;
    if (!matched) {
      const auto state = core.registers();
      std::cerr << "code=" << code.size() << " grant=" << grant << " accepted=" << accept
                << " initial_ccr=" << unsigned{trace.registers[0].ccr}
                << " initial_d=" << trace.registers[0].d << " initial_y=" << trace.registers[0].y
                << " actual_d=" << state.d << " expected_d=" << expected.d
                << " actual_y=" << state.y << " expected_y=" << expected.y
                << " actual_ccr=" << unsigned{state.ccr}
                << " expected_ccr=" << unsigned{expected.ccr} << " actual_pc=" << state.pc
                << " expected_pc=" << expected.pc << " observed=" << port.observed << '\n';
    }
    require(matched, "Resident prefix, physical aliases, value versions or owner state differ");
    if (refused) {
      auto failure = actual;
      failure.retired = 0;
      require(actual.address == external && actual.access_size == 2 && actual.value == offered &&
                  core.state().failure == failure,
              "Owner refusal must retain its original fault context");
    }
    ++checked;
  }
};

template <Form form> void cases() {
  auto code = program<form>();
  Device device(code);
  constexpr std::array<Address, 7> values{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xffff};
  for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto before = Registers{.pc = entry,
                                    .d = values[index],
                                    .x = seed_x,
                                    .y = values[(index + 3) % values.size()],
                                    .sp = stack,
                                    .ccr = static_cast<Byte>(ccr)};
      const auto trace = predict(before, form);
      // Enter the complete retained block before testing every smaller deadline.
      device.check(trace, code, trace.size + 1, false);
      device.check(trace, code, trace.size + 1, true);
      if (ccr == 0 && index == 0) {
        require(device.core.statistics().compiled_entries == 1,
                "Resident prefix and real owner must remain in one retained entry");
      }
      for (unsigned grant = 1; grant <= trace.size; ++grant) {
        device.check(trace, code, grant, false);
      }
    }
  }
}

void byte_layout() {
  auto code = isa::program(isa::opcode<isa::Opcode::ldab_immediate>(),
                           std::array<Byte, 1>{byte_b},
                           isa::opcode<isa::Opcode::ldaa_immediate>(),
                           std::array<Byte, 1>{byte_a},
                           isa::opcode<isa::Opcode::stab_extended>(),
                           address(data),
                           isa::opcode<isa::Opcode::staa_extended>(),
                           address(data + 1),
                           isa::opcode<isa::Opcode::ldx_extended>(),
                           address(alias),
                           isa::opcode<isa::Opcode::ldd_extended>(),
                           address(external));
  Device device(code);
  for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
    Trace trace;
    trace.registers[0] = {.pc = entry,
                          .d = seed_d,
                          .x = byte_seed_x,
                          .y = byte_seed_y,
                          .sp = stack,
                          .ccr = static_cast<Byte>(ccr)};
    trace.memory[0].fill(sentinel);
    auto state = trace.registers[0];
    state.d = (seed_d & upper_byte_mask) | byte_b;
    state.ccr = flags(state.ccr, byte_b, false);
    trace.append(state, trace.memory[0], 2);
    state = trace.registers.at(trace.size);
    state.d = (unsigned{byte_a} << byte_bits) | byte_b;
    state.ccr = flags(state.ccr, byte_a, false);
    trace.append(state, trace.memory[0], 2);
    trace.store(0, byte_b, false);
    trace.store(1, byte_a, false);
    state = trace.registers.at(trace.size);
    state.x = byte_word;
    state.ccr = flags(state.ccr, state.x);
    trace.append(state, trace.memory.at(trace.size), 3);
    device.check(trace, code, trace.size + 1, false);
    device.check(trace, code, trace.size + 1, true);
    for (unsigned grant = 1; grant <= trace.size; ++grant) {
      device.check(trace, code, grant, false);
    }
  }
}
}

int main() {
  try {
    cases<Form::plain>();
    cases<Form::d>();
    cases<Form::y>();
    cases<Form::byte>();
    cases<Form::word>();
    cases<Form::stale_y>();
    cases<Form::partial_d>();
    cases<Form::new_word>();
    byte_layout();
    std::cout << checked << " resident prefix, byte provenance and owner cases passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
