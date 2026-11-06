// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
using Opcode = isa::Opcode;
constexpr Address entry = 0x0200, data = 0x4000, alias = 0x5000, external = 0x6000;
constexpr Address scratch = data + 64, replacement = 0x8123, offered = 0x2468;
constexpr unsigned byte_bits = 8, byte_mask = 255, word_mask = 65535, sign = 32768;
constexpr unsigned words = 20, stride = 257, guard = 0xa5, preserved = 0xf0;
constexpr Address dead_word = alias + 44, live_word = alias + 46, sample = data + 20;
constexpr Address marker_x = 0x3456, marker_y = 0x5678, stack = 0x9000;
constexpr unsigned memory_bytes = 96;
using Bytes = std::array<Byte, memory_bytes>;
unsigned checked = 0;

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Operation {
  Opcode opcode;
  Address operand;
};

auto operations() {
  const std::array tail{Operation{.opcode = Opcode::cpd_extended,
                                  .operand = dead_word}, // Dead flags; no input cache.
                        Operation{.opcode = Opcode::addd_extended, .operand = data},
                        Operation{.opcode = Opcode::ldd_extended, .operand = dead_word},
                        Operation{.opcode = Opcode::ldd_immediate, .operand = replacement},
                        Operation{.opcode = Opcode::staa_extended, .operand = data + 3},
                        Operation{.opcode = Opcode::ldd_extended, .operand = alias + 2},
                        Operation{.opcode = Opcode::std_extended, .operand = data + 5},
                        Operation{.opcode = Opcode::ldy_extended, .operand = alias + 4},
                        Operation{.opcode = Opcode::addd_extended, .operand = alias + 6},
                        Operation{.opcode = Opcode::sty_extended, .operand = data + 7},
                        Operation{.opcode = Opcode::ldd_extended, .operand = alias + 6},
                        Operation{.opcode = Opcode::subd_extended, .operand = alias + 8},
                        Operation{.opcode = Opcode::std_extended, .operand = data + 2},
                        Operation{.opcode = Opcode::ldd_extended, .operand = sample},
                        Operation{.opcode = Opcode::addd_extended, .operand = alias + 2},
                        Operation{.opcode = Opcode::cpd_extended, .operand = live_word},
                        Operation{.opcode = Opcode::ldd_extended, .operand = live_word}};
  std::vector<Operation> result;
  result.reserve((std::size_t{words} * 3) + tail.size());
  for (unsigned index = 0; index < words; ++index) {
    result.push_back(
        {.opcode = Opcode::ldd_extended, .operand = static_cast<Address>(data + (index * 2))});
  }
  for (unsigned index = 0; index < words; ++index) {
    result.push_back(
        {.opcode = Opcode::addd_extended, .operand = static_cast<Address>(alias + (index * 2))});
    result.push_back({.opcode = Opcode::std_extended, .operand = scratch});
  }
  result.insert(result.end(), tail.begin(), tail.end());
  return result;
}

auto program(const std::span<const Operation> operations) {
  std::vector<Byte> result;
  result.reserve((operations.size() + 1) * 3);
  for (const auto operation : operations) {
    result.push_back(static_cast<Byte>(operation.opcode));
    result.push_back(static_cast<Byte>(operation.operand >> byte_bits));
    result.push_back(static_cast<Byte>(operation.operand & byte_mask));
  }
  result.push_back(static_cast<Byte>(Opcode::ldd_extended));
  result.push_back(static_cast<Byte>(external >> byte_bits));
  result.push_back(static_cast<Byte>(external & byte_mask));
  return result;
}

unsigned offset(const Address address) {
  const auto base = address >= alias ? alias : data;
  return static_cast<unsigned>(address - base);
}

Address word(const Bytes &bytes, const Address address) {
  const auto at = offset(address);
  return static_cast<Address>((unsigned{bytes.at(at)} << byte_bits) | bytes.at(at + 1));
}

void word(Bytes &bytes, const WordWrite write) {
  const auto at = offset(write.address);
  bytes.at(at) = static_cast<Byte>(write.value >> byte_bits);
  bytes.at(at + 1) = static_cast<Byte>(write.value & byte_mask);
}

Byte transfer_flags(const Byte ccr, const Address value, const bool wide = true) {
  constexpr unsigned retained = 0xf1;
  return static_cast<Byte>((ccr & retained) | (value == 0 ? 4U : 0U) |
                           (value >= (wide ? sign : 128U) ? 8U : 0U));
}

int signed_word(const Address value) {
  return value < sign ? int{value} : int{value} - int{word_mask + 1};
}

void arithmetic(Registers &state, const Address value, const bool subtract, const bool store) {
  const auto wide =
      subtract ? unsigned{state.d} + word_mask + 1U - value : unsigned{state.d} + value;
  const auto result = static_cast<Address>(wide & word_mask);
  const auto signed_result =
      signed_word(state.d) + (subtract ? -signed_word(value) : signed_word(value));
  const bool overflow = signed_result < -int{sign} || signed_result >= int{sign};
  const bool carry = subtract ? state.d < value : wide > word_mask;
  state.ccr =
      static_cast<Byte>((state.ccr & preserved) | (result == 0 ? 4U : 0U) |
                        (result >= sign ? 8U : 0U) | (overflow ? 2U : 0U) | (carry ? 1U : 0U));
  if (store) { state.d = result; }
}

struct Snapshot {
  Registers registers;
  Bytes bytes;
};

void apply(Snapshot &snapshot, const Operation operation) {
  auto &state = snapshot.registers;
  auto &bytes = snapshot.bytes;
  const auto address = operation.operand;
  const auto opcode = operation.opcode;
  state.pc += 3;
  if (opcode == Opcode::ldd_immediate) {
    state.d = address;
    state.ccr = transfer_flags(state.ccr, state.d);
    return;
  }
  if (opcode == Opcode::ldd_extended) {
    state.d = word(bytes, address);
    state.ccr = transfer_flags(state.ccr, state.d);
    return;
  }
  if (opcode == Opcode::ldy_extended) {
    state.y = word(bytes, address);
    state.ccr = transfer_flags(state.ccr, state.y);
    return;
  }
  if (opcode == Opcode::std_extended || opcode == Opcode::sty_extended) {
    const auto value = opcode == Opcode::std_extended ? state.d : state.y;
    word(bytes, {.address = address, .value = value});
    state.ccr = transfer_flags(state.ccr, value);
    return;
  }
  if (opcode == Opcode::staa_extended) {
    bytes.at(offset(address)) = static_cast<Byte>(state.d >> byte_bits);
    state.ccr = transfer_flags(state.ccr, static_cast<Address>(state.d >> byte_bits), false);
    return;
  }
  if (opcode == Opcode::addd_extended || opcode == Opcode::subd_extended ||
      opcode == Opcode::cpd_extended) {
    arithmetic(state,
               word(bytes, address),
               opcode != Opcode::addd_extended,
               opcode != Opcode::cpd_extended);
    return;
  }
  throw std::runtime_error("Unexpected independent fixture operation");
}

auto predict(const std::span<const Operation> operations, const Byte ccr, const Address seed) {
  Snapshot initial{
      .registers = {.pc = entry, .d = seed, .x = marker_x, .y = marker_y, .sp = stack, .ccr = ccr},
      .bytes = {}};
  initial.bytes.fill(guard);
  constexpr unsigned initialized_words = 24;
  for (unsigned index = 0; index < initialized_words; ++index) {
    word(initial.bytes,
         {.address = static_cast<Address>(data + (index * 2)),
          .value = static_cast<Address>((seed + index * stride) & word_mask)});
  }
  std::vector<Snapshot> trace{initial};
  trace.reserve(operations.size() + 1);
  for (const auto operation : operations) {
    auto next = trace.back();
    apply(next, operation);
    trace.push_back(next);
  }
  return trace;
}

struct Port {
  ObservedCore *core = nullptr;
  const Snapshot *expected = nullptr;
  const Bytes *bytes = nullptr;
  unsigned reads = 0;
  bool accept = false, observed = true;

  static bool read(void *owner, const Address at, Address &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    ++port.reads;
    port.observed = port.observed && at == external &&
                    port.core->registers() == port.expected->registers &&
                    *port.bytes == port.expected->bytes;
    value = offered;
    return port.accept;
  }
};

struct Device {
  Bytes bytes{};
  Port port{.bytes = &bytes};
  Io io{.owner = &port, .read_word = Port::read};
  std::array<Ram, 3> regions;
  ObservedCore core;

  explicit Device(const std::span<Byte> code)
      : regions{Ram{.first = entry, .bytes = code, .writable = false},
                Ram{.first = data, .bytes = bytes},
                Ram{.first = alias, .bytes = bytes, .writable = false}},
        core(Memory::mapped(regions), &io) {
    port.core = &core;
  }

  void check(const std::span<const Snapshot> trace, const unsigned grant, const bool accept) {
    const auto length = static_cast<unsigned>(trace.size() - 1);
    const bool owner = grant > length;
    const bool refused = owner && !accept;
    const auto before = owner ? length : grant;
    bytes = trace.front().bytes;
    port.expected = &trace.back();
    port.reads = 0;
    port.observed = true;
    port.accept = accept;
    core.set_registers(trace.front().registers);
    const auto actual = core.run(grant);
    auto expected = trace[before].registers;
    if (owner && accept) {
      expected.d = offered;
      expected.ccr = transfer_flags(expected.ccr, offered);
      expected.pc += 3;
    }
    const auto at = owner ? trace.back().registers.pc : trace[grant - 1].registers.pc;
    require(actual.retired == grant - (refused ? 1U : 0U) && actual.pc == at &&
                actual.opcode == regions[0].bytes[at - entry] &&
                actual.fault == (refused ? Fault::memory_read : Fault::none) &&
                actual.state == ExecutionState::running && core.registers() == expected &&
                bytes == trace[before].bytes && port.reads == (owner ? 1U : 0U) && port.observed,
            "Resident words differ across pressure, aliases, overlaps, dead reads or deadlines");
    if (refused) {
      auto failure = actual;
      failure.retired = 0;
      require(actual.address == external && actual.access_size == 2 && actual.value == offered &&
                  core.state().failure == failure,
              "Owner failure metadata or latch differs");
    } else {
      require(core.state().failure == RunResult{},
              "Successful native values must not hide a fault");
    }
    ++checked;
  }
};
}

int main() {
  try {
    const auto ops = operations();
    auto code = program(ops);
    Device device(code);
    constexpr std::array<Address, 2> seeds{0x1357, 0x8000};
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      for (const auto seed : seeds) {
        const auto trace = predict(ops, static_cast<Byte>(ccr), seed);
        const auto length = static_cast<unsigned>(ops.size());
        device.check(trace, length + 1, false);
        device.check(trace, length + 1, true);
        for (unsigned grant = 1; grant <= length; ++grant) { device.check(trace, grant, false); }
      }
    }
    require(code == program(ops),
            "Resident data allocation must not modify original program bytes");
    std::cout << checked << " resident word pressure, alias, overwrite and deadline cases passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
