// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "reference.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace cpu12::test;
constexpr unsigned address_bytes = 65536;
constexpr unsigned direct_delta = 0x10;
constexpr unsigned extended_delta = 0x30;
constexpr unsigned a_store = 0x5a;
constexpr unsigned b_store = 0x5b;
constexpr unsigned store_extended_delta = 0x20;
constexpr unsigned byte_bits = 8;
constexpr std::uint16_t direct_address = 0x90;
constexpr std::uint16_t extended_address = 0xff90;
constexpr std::uint16_t other_address = 0x1290;
std::uint64_t checked = 0;

void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

struct Encoding {
  std::array<std::uint8_t, 3> code{};
  std::size_t length = 0;

  [[nodiscard]] std::span<const std::uint8_t> view() const { return std::span(code).first(length); }
};

struct Access {
  unsigned opcode;
  std::uint16_t address;
  bool extended;
};

Encoding encode(const Access access) {
  const auto high = static_cast<std::uint8_t>(access.address >> byte_bits);
  const auto low = static_cast<std::uint8_t>(access.address & byte_mask);
  return {.code = {static_cast<std::uint8_t>(access.opcode), access.extended ? high : low, low},
          .length = access.extended ? 3U : 2U};
}

struct ReadCase {
  Input input;
  std::uint16_t address;
  bool extended;
  std::uint16_t pc = seed.pc;
};

void read_case(cpu12::Core &core, const ReadCase &test) {
  const bool b = (test.input.opcode & 0x40U) != 0;
  auto before = seed;
  before.pc = test.pc;
  before.ccr = static_cast<std::uint8_t>(test.input.ccr);
  before.d = static_cast<std::uint16_t>(b ? other_a | test.input.left
                                          : (test.input.left * values) | other_b);
  auto wanted = before;
  const auto oracle = expected(test.input);
  const auto instruction =
      encode({.opcode = test.input.opcode + (test.extended ? extended_delta : direct_delta),
              .address = test.address,
              .extended = test.extended});
  wanted.pc = static_cast<std::uint16_t>(wanted.pc + instruction.length);
  wanted.ccr = static_cast<std::uint8_t>(oracle.ccr);
  if (oracle.store) {
    wanted.d = static_cast<std::uint16_t>(b ? (before.d & high_mask) | oracle.value
                                            : (oracle.value * values) | (before.d & byte_mask));
  }
  core.set_registers(before);
  const auto result = core.run_instruction(instruction.view());
  require(result.retired == 1 && result.fault == cpu12::Fault::none && result.pc == before.pc &&
              core.registers() == wanted,
          "Independent memory-operand arithmetic, flags and complete state expectation failed");
  ++checked;
}

void ram_arithmetic() {
  std::array<std::uint8_t, address_bytes> ram{};
  cpu12::Core core(cpu12::Memory::unified(ram));
  constexpr std::array<unsigned, 16> opcodes{0x86,
                                             0xc6,
                                             0x8b,
                                             0xcb,
                                             0x80,
                                             0xc0,
                                             0x81,
                                             0xc1,
                                             0x84,
                                             0xc4,
                                             0x8a,
                                             0xca,
                                             0x88,
                                             0xc8,
                                             0x85,
                                             0xc5};
  for (const bool extended : {false, true}) {
    const auto address = extended ? extended_address : direct_address;
    for (const auto opcode : opcodes) {
      for (unsigned right = 0; right < values; ++right) {
        ram[address] = static_cast<std::uint8_t>(right);
        for (unsigned left = 0; left < values; ++left) {
          read_case(core,
                    {.input = {.opcode = opcode, .left = left, .right = right, .ccr = byte_mask},
                     .address = address,
                     .extended = extended});
        }
        require(ram[address] == right, "Read operands never write their source RAM");
      }
    }
  }
  require(core.statistics().native_instructions == checked &&
              core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Known RAM operations execute natively without bus callbacks");
}

struct StoreCase {
  std::uint16_t address;
  bool extended, b;
  unsigned value;
};

void store_case(cpu12::Core &core, const std::span<const std::uint8_t> ram, const StoreCase test) {
  const auto address = test.address;
  const auto value = test.value;
  const auto b = test.b;
  const auto instruction =
      encode({.opcode = (b ? b_store : a_store) + (test.extended ? store_extended_delta : 0U),
              .address = address,
              .extended = test.extended});
  auto before = seed;
  before.ccr = byte_mask;
  before.d = static_cast<std::uint16_t>(b ? other_a | value : (value * values) | other_b);
  auto wanted = before;
  wanted.pc = static_cast<std::uint16_t>(wanted.pc + instruction.length);
  wanted.ccr =
      static_cast<std::uint8_t>((before.ccr & (byte_mask ^ nzv)) | (value == 0 ? zero : 0U) |
                                (value >= sign_bit ? negative : 0U));
  core.set_registers(before);
  const auto result = core.run_instruction(instruction.view());
  require(result.retired == 1 && core.registers() == wanted && ram[address] == value &&
              ram[address - 1] == 0 && ram[address + 1] == 0,
          "Stores retain A/B, set independent NZV flags and touch exactly one byte");
  ++checked;
}

void ram_stores() {
  std::array<std::uint8_t, address_bytes> ram{};
  cpu12::Core core(cpu12::Memory::unified(ram));
  for (const bool extended : {false, true}) {
    const auto address = extended ? extended_address : direct_address;
    for (const bool b : {false, true}) {
      for (unsigned value = 0; value < values; ++value) {
        store_case(core, ram, {.address = address, .extended = extended, .b = b, .value = value});
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Direct stores make no bus calls");
}

struct Bus {
  std::uint16_t address = extended_address, last_address = 0;
  std::uint8_t value = 0;
  unsigned reads = 0, writes = 0;
  bool reject_read = false, reject_write = false, reenter = false;
  bool recursive_refused = false, registers_refused = false;
  cpu12::Core *core = nullptr;
  cpu12::Registers at_access;
  static bool read(void *owner, std::uint16_t address, std::uint8_t &value) noexcept;
  static bool write(void *owner, cpu12::ByteWrite access) noexcept;
};

bool Bus::read(void *const owner, const std::uint16_t address, std::uint8_t &value) noexcept {
  auto &bus = *static_cast<Bus *>(owner);
  ++bus.reads;
  bus.last_address = address;
  bus.at_access = bus.core->registers();
  if (bus.reenter) {
    constexpr std::array<std::uint8_t, 1> nop{0xa7};
    const auto result = bus.core->run_instruction(nop);
    bus.recursive_refused = result.retired == 0 && result.fault == cpu12::Fault::reentrant;
    try {
      bus.core->set_registers({});
    } catch (const std::logic_error &) { bus.registers_refused = true; }
  }
  value = bus.value;
  return address == bus.address && !bus.reject_read;
}

bool Bus::write(void *const owner, const cpu12::ByteWrite access) noexcept {
  const auto address = access.address;
  const auto value = access.value;
  auto &bus = *static_cast<Bus *>(owner);
  ++bus.writes;
  bus.last_address = address;
  bus.at_access = bus.core->registers();
  if (address != bus.address) { return false; }
  // A refused operation may already have an observable device side effect.
  bus.value = value;
  return !bus.reject_write;
}

void bus_operations() {
  Bus bus;
  cpu12::Io io{.owner = &bus, .read = &Bus::read, .write = &Bus::write};
  cpu12::Core core(cpu12::Memory::external(), &io);
  bus.core = &core;
  constexpr std::array<unsigned, 8> opcodes{0x86, 0xc6, 0x8b, 0xcb, 0x80, 0xc0, 0x81, 0xc1};
  constexpr std::array<unsigned, 5> inputs{0, 1, 127, 128, 255};
  for (const bool extended : {false, true}) {
    bus.address = extended ? extended_address : direct_address;
    for (const auto opcode : opcodes) {
      for (const auto value : inputs) {
        const auto reads = bus.reads;
        bus.value = static_cast<std::uint8_t>(value);
        read_case(
            core,
            {.input = {.opcode = opcode, .left = sign_bit - 1, .right = value, .ccr = byte_mask},
             .address = bus.address,
             .extended = extended});
        require(bus.reads == reads + 1 && bus.last_address == bus.address &&
                    bus.at_access.pc == seed.pc && bus.at_access.ccr == byte_mask,
                "A genuine bus read happens once before register/flag/PC commit");
      }
    }
  }
  bus.address = extended_address;
  bus.reenter = true;
  bus.value = 1;
  read_case(core,
            {.input = {.opcode = load_a, .left = 0, .right = 1, .ccr = 0},
             .address = bus.address,
             .extended = true});
  require(bus.recursive_refused && bus.registers_refused,
          "Callbacks cannot reenter or free/change active CPU state/code");
  bus.reenter = false;
  bus.reject_write = true;
  core.set_registers(seed);
  const auto instruction =
      encode({.opcode = a_store + store_extended_delta, .address = bus.address, .extended = true});
  const auto fault = core.run_instruction(instruction.view());
  const auto writes = bus.writes;
  const auto retired = core.statistics().native_instructions;
  require(fault.fault == cpu12::Fault::memory_write && fault.retired == 0 && fault.pc == seed.pc &&
              fault.address == bus.address && fault.value == (seed.d >> byte_bits) &&
              bus.value == fault.value && core.registers() == seed,
          "A real refused bus write retains its side effect and reports exact context without CPU "
          "retirement");
  const auto repeated = core.run_instruction(instruction.view());
  require(
      repeated.fault == fault.fault && repeated.address == fault.address && bus.writes == writes &&
          core.statistics().native_instructions == retired,
      "A latched fault prevents silent duplicate bus traffic across repeated dispatch attempts");
  bus.value = 0;
  bus.reject_write = false;
  core.set_registers(seed); // Caller explicitly restores both bus and CPU boundary.
  const auto success = core.run_instruction(instruction.view());
  require(success.retired == 1 && bus.writes == writes + 1 && bus.value == (seed.d >> byte_bits) &&
              core.registers().pc == 1,
          "An explicit fresh boundary permits one actual successful write");
  bus.reject_read = true;
  core.set_registers(seed);
  const auto load =
      encode({.opcode = load_a + extended_delta, .address = bus.address, .extended = true});
  const auto failed = core.run_instruction(load.view());
  const auto reads = bus.reads;
  require(failed.fault == cpu12::Fault::memory_read && failed.address == bus.address &&
              failed.retired == 0 && core.registers() == seed,
          "Read refusal leaves all architectural registers unchanged");
  require(core.run_instruction(load.view()).retired == 0 && bus.reads == reads,
          "Refused reads are not replayed");
  require(core.statistics().read_callbacks == bus.reads &&
              core.statistics().write_callbacks == bus.writes,
          "Callback statistics count actual calls, including refusals");
}

void boundaries() {
  std::array<std::uint8_t, 1> byte{0x80};
  constexpr std::uint16_t last = 0xffff;
  cpu12::Core core(cpu12::Memory::window({.first = last, .bytes = byte}));
  read_case(core,
            {.input = {.opcode = load_a, .left = 0, .right = sign_bit, .ccr = 0},
             .address = last,
             .extended = true,
             .pc = 0});
  const auto instruction =
      encode({.opcode = load_a + extended_delta, .address = 0, .extended = true});
  core.set_registers(seed);
  const auto result = core.run_instruction(instruction.view());
  require(result.fault == cpu12::Fault::memory_read && result.address == 0 &&
              result.pc == seed.pc && core.registers() == seed,
          "A RAM window ending at ffff does not wrap into unknown address zero");
  std::array<std::uint8_t, 2> oversized{};
  bool rejected = false;
  try {
    const cpu12::Core invalid(cpu12::Memory::window({.first = last, .bytes = oversized}));
  } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "RAM windows cannot exceed the 16-bit address space");
  std::array<std::uint8_t, address_bytes> ram{};
  cpu12::Core cached(cpu12::Memory::unified(ram));
  constexpr std::array<std::uint16_t, 3> addresses{direct_address, other_address, direct_address};
  unsigned value = 0;
  for (const auto address : addresses) {
    ram[address] = static_cast<std::uint8_t>(++value);
    read_case(cached,
              {.input = {.opcode = load_a, .left = 0, .right = value, .ccr = 0},
               .address = address,
               .extended = true,
               .pc = 0});
  }
  require(cached.statistics().compiled_entries == 2,
          "Extended cache keys retain both address bytes and read current RAM contents");
}
}

int main() {
  try {
    require(cpu12::Core::native_supported(), "Native host required");
    ram_arithmetic();
    ram_stores();
    bus_operations();
    boundaries();
    std::cout << "Verified " << checked
              << " native memory cases plus exact bus faults, callbacks, reentrancy and RAM/cache "
                 "boundaries.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
