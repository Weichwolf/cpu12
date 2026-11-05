// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fetch_model.hpp"

#include <array>
#include <stdexcept>

namespace cpu12::test::fetching {
namespace {
constexpr Registers seed{.pc = 0xfffe, .d = 0x1234, .x = 0x8000, .sp = 0x7000, .ccr = 0xa1};

struct Bus {
  std::array<Byte, maximum_capture_bytes> code{0x18, 4, 0x12, 0x34, 0x56, 0x78};
  unsigned length = maximum_capture_bytes, failure = maximum_capture_bytes;
  unsigned calls = 0, reads = 0;
  std::array<Address, 16> addresses{};
  Core *core = nullptr;
  bool guarded = true;
  bool read_accepted = true;

  void reentry() noexcept {
    constexpr std::array<Byte, 1> nop{0xa7};
    guarded &= core->step().fault == Fault::reentrant && core->run(0).fault == Fault::reentrant &&
               core->run_instruction(nop).fault == Fault::reentrant;
    bool registers = false;
    try {
      core->set_registers({});
    } catch (const std::logic_error &) { registers = true; }
    guarded &= registers;
  }

  static bool fetch(void *owner, const Address address, Byte &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    if (bus.core != nullptr) { bus.reentry(); }
    if (bus.calls < bus.addresses.size()) { bus.addresses[bus.calls] = address; }
    ++bus.calls;
    const auto offset = static_cast<Address>(address - seed.pc);
    if (offset >= bus.length) { return false; }
    value = bus.code[offset];
    return offset != bus.failure;
  }

  static bool read(void *owner, Address /*address*/, Byte &value) noexcept {
    ++static_cast<Bus *>(owner)->reads;
    constexpr Byte payload = 0x5a;
    value = payload;
    return static_cast<Bus *>(owner)->read_accepted;
  }
};

void failed_phases() {
  for (unsigned phase = 0; phase < maximum_capture_bytes; ++phase) {
    Bus bus{.failure = phase};
    Io io{.owner = &bus, .read = Bus::read, .fetch = Bus::fetch};
    Core core(Memory::external(), &io);
    core.set_registers(seed);
    const auto result = core.step();
    require(
        result.fault == Fault::instruction_fetch && result.retired == 0 && result.pc == seed.pc &&
            result.opcode == (phase == 0 ? 0 : bus.code[0]) &&
            result.address == static_cast<Address>(seed.pc + phase) &&
            result.value == bus.code[phase] && result.access_size == 1 &&
            core.registers() == seed && bus.calls == phase + 1 && bus.reads == 0,
        "Every refused fetch phase reports actual byte/address/original PC without data access");
    for (unsigned index = 0; index <= phase; ++index) {
      require(bus.addresses[index] == static_cast<Address>(seed.pc + index),
              "Fetch callback effects retain exact byte order across address wrap");
    }
    require(core.run(10).fault == Fault::instruction_fetch && bus.calls == phase + 1 &&
                core.statistics().native_instructions == 0,
            "A fetch fault remains latched across repeated calls without replay");
    constexpr std::array<Byte, 1> nop{0xa7};
    core.set_registers(seed);
    require(core.run_instruction(nop).retired == 1 && bus.calls == phase + 1,
            "Explicit new registers recover captured native execution without fetching");
    ++native;
    ++captures;
  }
}

void unsupported() {
  constexpr std::array<std::array<Byte, 2>, 4> code{
      {{0, 0}, {0x18, 0xff}, {0x19, 0xe3}, {0x0c, 0xff}}};
  for (const auto bytes : code) {
    Bus bus;
    bus.code[0] = bytes[0];
    bus.code[1] = bytes[1];
    Io io{.owner = &bus, .fetch = Bus::fetch};
    Core core(Memory::external(), &io);
    core.set_registers(seed);
    const auto result = core.step();
    const unsigned fetched = bytes[0] == 0 ? 1 : 2;
    require(result.fault == Fault::unsupported_instruction && result.retired == 0 &&
                result.pc == seed.pc && result.opcode == bytes[0] && core.registers() == seed &&
                bus.calls == fetched,
            "Unknown opcode/page or excluded indexed pointer stops before unrelated code bytes");
    require(core.step().fault == result.fault && bus.calls == fetched,
            "Unsupported fetched input latches instead of repeating its real fetch effects");
    ++captures;
  }
}

void separation() {
  Bus bus;
  Io data_only{.owner = &bus, .read = Bus::read};
  Core core(Memory::external(), &data_only);
  core.set_registers(seed);
  require(core.run(0).retired == 0 && core.run(0).fault == Fault::none && bus.reads == 0,
          "Zero instruction budget has no memory or code effects");
  require(core.step().fault == Fault::instruction_fetch && bus.reads == 0 && bus.calls == 0,
          "Absent fetch transport must never fall back to a data callback");
  Bus guarded;
  constexpr Byte nop = 0xa7;
  guarded.code[0] = nop;
  guarded.length = 1;
  Io io{.owner = &guarded, .fetch = Bus::fetch};
  Core target(Memory::external(), &io);
  guarded.core = &target;
  target.set_registers(seed);
  require(target.step().retired == 1 && guarded.guarded && guarded.calls == 1 &&
              target.statistics().fetch_callbacks == 1,
          "Fetch callbacks reject recursive execution and register mutation");
  ++native;
}

void seams() {
  for (unsigned position = 0; position < 3; ++position) {
    Bus bus;
    constexpr std::array<Byte, maximum_capture_bytes> code{0xce, 0xab, 0xcd};
    bus.code = code;
    bus.length = 3;
    std::array<Byte, 1> known{bus.code[position]};
    const auto first = static_cast<Address>(seed.pc + position);
    Io io{.owner = &bus, .fetch = Bus::fetch};
    Core core(Memory::window({.first = first, .bytes = known}), &io);
    core.set_registers(seed);
    auto expected = seed;
    expected.pc = 1;
    constexpr Address loaded = 0xabcd;
    expected.x = loaded;
    constexpr Byte flags = 0xa9;
    expected.ccr = flags;
    require(core.step().retired == 1 && core.registers() == expected && bus.calls == 2 &&
                core.statistics().fetch_callbacks == 2,
            "Code bytes cross below/inside/above a partial window with exact external fetches");
    ++native;
  }
}

void data_failure() {
  Bus bus{.read_accepted = false};
  constexpr std::array<Byte, maximum_capture_bytes> code{0xb6, 0xff, 0x90};
  bus.code = code;
  bus.length = 3;
  Io io{.owner = &bus, .read = Bus::read, .fetch = Bus::fetch};
  Core core(Memory::external(), &io);
  core.set_registers(seed);
  const auto result = core.step();
  constexpr Address port = 0xff90;
  constexpr unsigned refused_value = 0x5a;
  require(result.retired == 0 && result.fault == Fault::memory_read && result.address == port &&
              result.value == refused_value && result.access_size == 1 &&
              core.registers() == seed && bus.calls == 3 && bus.reads == 1,
          "Successful fetch precedes one real refused data access without native retirement");
  require(core.step().fault == Fault::memory_read && bus.calls == 3 && bus.reads == 1,
          "Latched data failure replays neither its fetch nor its data callback");
  ++captures;
}
}

void bus_cases() {
  failed_phases();
  unsupported();
  separation();
  seams();
  data_failure();
}
}
