// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "observed_core.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
constexpr Registers seed{
    .pc = 0x0200, .d = 0xa55a, .x = 0x3456, .y = 0x5678, .sp = 0x9000, .ccr = 0};
constexpr Address offered = 0x8001, stored = 0xa55a, loaded = 0x1234, sign = 0x8000;
constexpr unsigned byte_bits = 8, byte_mask = 255;
constexpr Byte retained = 0xf1, postincrement_two = 0x31;
enum class Mode : Byte { indexed, direct, extended };

struct Layout {
  Address address;
  bool split = false, gap = false, read_only = false, knowledge = false;
};

struct Access {
  Mode mode;
  bool write, accept;
};

struct Port {
  ObservedCore *core = nullptr;
  Registers expected;
  Address address = 0, value = 0;
  unsigned reads = 0, writes = 0;
  bool accept = false, observed = true;

  static bool read(void *owner, const Address at, Address &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.address = at;
    ++port.reads;
    port.observed = port.observed && port.core->registers() == port.expected;
    value = offered;
    return port.accept;
  }

  static bool write(void *owner, const WordWrite data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.address = data.address;
    port.value = data.value;
    ++port.writes;
    port.observed = port.observed && port.core->registers() == port.expected;
    return port.accept;
  }
};

struct Encoded {
  std::array<Byte, 3> bytes;
  unsigned length;
};

Encoded instruction(const Layout layout, const Access access) {
  if (access.mode == Mode::indexed) {
    const auto opcode = access.write ? isa::Opcode::std_indexed : isa::Opcode::ldd_indexed;
    return {.bytes = {static_cast<Byte>(opcode), postincrement_two, 0}, .length = 2};
  }
  if (access.mode == Mode::direct) {
    const auto opcode = access.write ? isa::Opcode::std_direct : isa::Opcode::ldd_direct;
    return {.bytes = {static_cast<Byte>(opcode), static_cast<Byte>(layout.address), 0},
            .length = 2};
  }
  const auto opcode = access.write ? isa::Opcode::std_extended : isa::Opcode::ldd_extended;
  return {.bytes = {static_cast<Byte>(opcode),
                    static_cast<Byte>(layout.address >> byte_bits),
                    static_cast<Byte>(layout.address & byte_mask)},
          .length = 3};
}

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Storage {
  std::array<Byte, 6> bytes{0xa5, 0x12, 0x34, 0x80, 0x34, 0x5a};
  std::array<Byte, 6> known{0, 1, 0, 0, 0, 0};
};

auto grants(Storage &storage, const Layout layout) {
  const unsigned low = layout.gap ? 4 : 2;
  const auto width = layout.split ? 1U : 2U;
  const auto known =
      layout.knowledge ? std::span(storage.known).subspan(1, width) : std::span<Byte>{};
  std::array<Ram, 2> result{Ram{.first = layout.address,
                                .bytes = std::span(storage.bytes).subspan(1, width),
                                .known = known,
                                .writable = !layout.read_only},
                            Ram{}};
  if (layout.split) {
    result[1] = {.first = static_cast<Address>(layout.address + 1U),
                 .bytes = std::span(storage.bytes).subspan(low, 1),
                 .known = layout.knowledge ? std::span(storage.known).subspan(low, 1)
                                           : std::span<Byte>{},
                 .writable = !layout.read_only};
  }
  return result;
}

struct Expected {
  Registers registers;
  Storage storage;
  bool admitted, success;
};

Expected predict(const Registers before, const Layout layout, const Access access) {
  const bool admitted =
      !layout.split && !(access.write && layout.read_only) && (access.write || !layout.knowledge);
  Expected result{.registers = before,
                  .storage = {},
                  .admitted = admitted,
                  .success = admitted || access.accept};
  auto &wanted = result.registers;
  Address value = stored;
  if (!access.write) { value = admitted ? loaded : offered; }
  if (result.success) {
    wanted.pc = static_cast<Address>(before.pc + (access.mode == Mode::extended ? 3U : 2U));
    if (!access.write) { wanted.d = value; }
    if (access.mode == Mode::indexed) { wanted.x = static_cast<Address>(before.x + 2U); }
    wanted.ccr = static_cast<Byte>((before.ccr & retained) | (value == 0 ? 4U : 0U) |
                                   (value >= sign ? 8U : 0U));
  }
  if (admitted && access.write) {
    result.storage.bytes[1] = static_cast<Byte>(stored >> byte_bits);
    result.storage.bytes[2] = static_cast<Byte>(stored & byte_mask);
    if (layout.knowledge) { result.storage.known[1] = result.storage.known[2] = 1; }
  }
  return result;
}

void check_result(const ObservedCore &core,
                  const Port &port,
                  const RunResult result,
                  const Layout layout,
                  const Access access,
                  const bool admitted) {
  require(port.observed && port.reads == static_cast<unsigned>(!admitted && !access.write) &&
              port.writes == static_cast<unsigned>(!admitted && access.write),
          "One word access must invoke its actual owner once with the original CPU state");
  if (!admitted) {
    require(port.address == layout.address && (!access.write || port.value == stored),
            "The word owner must receive the original address and complete store value");
  }
  require(result.pc == seed.pc && result.opcode == instruction(layout, access).bytes[0] &&
              result.state == ExecutionState::running,
          "Word access result must identify the original instruction and execution state");
  if (admitted || access.accept) {
    require(result.fault == Fault::none && core.state().failure == RunResult{},
            "A successful word access must not leave a hidden fault");
    return;
  }
  require(result.fault == (access.write ? Fault::memory_write : Fault::memory_read) &&
              result.address == layout.address && result.access_size == 2 &&
              result.value == (access.write ? stored : offered) && core.state().failure == result,
          "A refused word must report its actual width, address, value and latched fault");
}

unsigned exercise(const Layout layout, const Access access) {
  Storage storage;
  auto regions = grants(storage, layout);
  Port port;
  Io io{.owner = &port, .read_word = &Port::read, .write_word = &Port::write};
  ObservedCore core(Memory::mapped(regions), &io);
  const auto code = instruction(layout, access);
  constexpr unsigned flags_count = 256;
  for (unsigned ccr = 0; ccr < flags_count; ++ccr) {
    storage = {};
    auto before = seed;
    before.ccr = static_cast<Byte>(ccr);
    if (access.mode == Mode::indexed) { before.x = layout.address; }
    core.set_registers(before);
    port = {.core = &core, .expected = before, .accept = access.accept};
    const auto expected = predict(before, layout, access);
    const auto result = core.run_instruction(std::span(code.bytes).first(code.length));
    if (result.retired != static_cast<unsigned>(expected.success) ||
        core.registers() != expected.registers || storage.bytes != expected.storage.bytes ||
        storage.known != expected.storage.known) {
      std::cerr << "address=" << layout.address
                << " mode=" << unsigned{static_cast<Byte>(access.mode)} << " write=" << access.write
                << " split=" << layout.split << " accepted=" << access.accept
                << " retired=" << result.retired
                << " fault=" << unsigned{static_cast<Byte>(result.fault)} << '\n';
      throw std::runtime_error(
          "Word access crossed a grant, skipped its owner or published wrong state");
    }
    check_result(core, port, result, layout, access, expected.admitted);
  }
  return flags_count;
}

unsigned read_only_byte_boundary() {
  constexpr Address at = 0x40;
  constexpr Address external = 0x6000;
  auto program = isa::program(isa::opcode<isa::Opcode::ldaa_extended>(),
                              std::array<Byte, 2>{0, at},
                              isa::opcode<isa::Opcode::ldd_extended>(),
                              std::array<Byte, 2>{external >> byte_bits, external & byte_mask});
  std::array<Byte, 1> data{};
  const std::array regions{Ram{.first = seed.pc, .bytes = program, .writable = false},
                           Ram{.first = at, .bytes = data, .writable = false}};
  Port port;
  Io io{.owner = &port, .read_word = &Port::read};
  ObservedCore core(Memory::mapped(regions), &io);
  for (unsigned value = 0; value <= byte_mask; ++value) {
    data[0] = static_cast<Byte>(value);
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      auto before = seed;
      before.ccr = static_cast<Byte>(ccr);
      auto wanted = before;
      wanted.pc = seed.pc + 3;
      wanted.d = static_cast<Address>((value << byte_bits) | (seed.d & byte_mask));
      constexpr unsigned byte_sign = 0x80;
      wanted.ccr = static_cast<Byte>((ccr & retained) | (value == 0 ? 4U : 0U) |
                                     (value >= byte_sign ? 8U : 0U));
      core.set_registers(before);
      port = {.core = &core, .expected = wanted};
      const auto result = core.run(2);
      require(result.retired == 1 && result.fault == Fault::memory_read &&
                  result.address == external && result.access_size == 2 && result.pc == wanted.pc &&
                  result.opcode == isa::opcode<isa::Opcode::ldd_extended>()[0] &&
                  core.registers() == wanted && port.observed && port.reads == 1 &&
                  port.writes == 0,
              "A real owner after a read-only byte load must see its actual value, flags and PC");
      auto failure = result;
      failure.retired = 0;
      require(core.state().failure == failure && data[0] == value,
              "An owner fault must retain its own metadata without altering the read-only byte");
    }
  }
  constexpr unsigned cases = (byte_mask + 1U) * (byte_mask + 1U);
  require(core.statistics().compiled_entries == 1 && core.statistics().native_instructions == cases,
          "Live read-only byte values must use one retained logical block without recompilation");
  return cases;
}
}

int main() {
  try {
    constexpr std::array layouts{
        Layout{.address = 0x40},
        Layout{.address = 0x40, .read_only = true},
        Layout{.address = 0x40, .knowledge = true},
        Layout{.address = 0x40, .split = true},
        Layout{.address = 0x40, .split = true, .gap = true},
        Layout{.address = 0xfffe},
        Layout{.address = 0xffff, .split = true},
        Layout{.address = 0xffff, .split = true, .gap = true, .read_only = true}};
    unsigned checked = 0;
    for (const auto layout : layouts) {
      for (const auto mode : {Mode::indexed, Mode::direct, Mode::extended}) {
        if (mode == Mode::direct && layout.address > byte_mask) { continue; }
        for (const bool write : {false, true}) {
          for (const bool accept : {false, true}) {
            checked += exercise(layout, {.mode = mode, .write = write, .accept = accept});
          }
        }
      }
    }
    const auto bytes = read_only_byte_boundary();
    std::cout << checked << " word ownership cases and " << bytes
              << " read-only byte/owner boundaries passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
