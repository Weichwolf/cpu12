// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/isa.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

static_assert(
    std::is_same_v<decltype(std::declval<const cpu12::Core &>().state()), const cpu12::State &>);

void live_state() {
  std::array<cpu12::Byte, 64> bytes{};
  cpu12::Core core(cpu12::Memory::unified(bytes));
  const auto &view = core.state();
  const auto *identity = &view;
  const cpu12::Registers before{.pc = 0x1234, .d = 0x5678, .sp = 0x8000, .ccr = 0xa5};
  core.set_registers(before);
  require(&core.state() == identity && view.registers == before,
          "State is a stable read-only view of the actual Core-owned register storage");
  const auto nop = core.run_instruction(cpu12::isa::nop);
  auto after = before;
  ++after.pc;
  require(nop.retired == 1 && view.registers == after &&
              view.execution == cpu12::ExecutionState::running && &core.state() == identity,
          "The same state reference observes native execution without a getter-side copy");
  constexpr cpu12::Address missing = 0xff80;
  constexpr auto load = cpu12::isa::program(cpu12::isa::opcode<cpu12::isa::Opcode::ldaa_extended>(),
                                            std::array<cpu12::Byte, 2>{0xff, 0x80});
  const auto failed = core.run_instruction(load);
  require(
      failed.fault == cpu12::Fault::memory_read && failed.address == missing &&
          view.failure == failed && view.registers == after && &core.state() == identity,
      "The live state also exposes the actual latched native failure without changing identity");
}

void unified() {
  constexpr unsigned data_address = 16;
  constexpr std::uint16_t first_value = 0x1200;
  constexpr std::uint16_t second_value = 0x3400;
  constexpr auto load_extended = cpu12::isa::opcode<cpu12::isa::Opcode::ldaa_extended>();
  std::array<cpu12::Byte, 64> bytes{load_extended[0], 0, data_address};
  auto memory = cpu12::Memory::unified(bytes);
  require(memory.storage().bytes.data() == bytes.data() && memory.storage().first == 0,
          "Unified memory is a borrowed byte view without copying");
  cpu12::Core core(memory);
  bytes[data_address] = static_cast<std::uint8_t>(first_value >> 8U);
  auto result = core.run_instruction(memory.storage().bytes.first(3));
  require(result.retired == 1 && core.registers().d == first_value && core.registers().pc == 3,
          "Code and data share one declared byte backing");
  bytes[data_address] = static_cast<std::uint8_t>(second_value >> 8U);
  core.set_registers({});
  result = core.run_instruction(memory.storage().bytes.first(3));
  require(result.retired == 1 && core.registers().d == second_value &&
              core.statistics().compiled_entries == 1,
          "Cached native code reads current shared RAM");
}

void retained_code() {
  constexpr std::uint16_t entries = 1152;
  constexpr unsigned byte_bits = 8;
  constexpr unsigned byte_mask = 0xff;
  constexpr unsigned instruction_bytes = 3;
  constexpr cpu12::Address stack = 0x9000;
  constexpr unsigned program_bytes = (entries * instruction_bytes) + 2;
  constexpr auto load = cpu12::isa::opcode<cpu12::isa::Opcode::ldd_immediate>();
  std::array<cpu12::Byte, cpu12::address_bytes> bytes{};
  for (std::uint16_t value = 0; value < entries; ++value) {
    const auto offset = value * instruction_bytes;
    bytes[offset] = load[0];
    bytes[offset + 1] = static_cast<cpu12::Byte>(value >> byte_bits);
    bytes[offset + 2] = static_cast<cpu12::Byte>(value & byte_mask);
  }
  bytes[program_bytes - cpu12::isa::stop.size()] = cpu12::isa::stop[0];
  bytes[program_bytes - 1] = cpu12::isa::stop[1];
  cpu12::Core core(cpu12::Memory::unified(bytes));
  for (unsigned pass = 0; pass < 2; ++pass) {
    core.set_registers({.sp = stack});
    const auto result = core.run();
    require(result.retired == entries + 1U && result.fault == cpu12::Fault::none &&
                result.state == cpu12::ExecutionState::stopped &&
                core.registers().d == entries - 1U && core.registers().pc == program_bytes &&
                core.statistics().compiled_entries == entries + 1U,
            "A complete unchanged program beyond the old cache capacity is translated only once");
  }
  require(core.statistics().native_instructions == (std::uint64_t{entries} + 1U) * 2U,
          "Both full program passes retain exact native retirement accounting");
}

struct Device {
  unsigned reads = 0;

  static bool read(void *owner, std::uint16_t address, std::uint8_t &value) noexcept {
    auto &device = *static_cast<Device *>(owner);
    ++device.reads;
    constexpr std::uint16_t port = 0xff90;
    constexpr std::uint8_t payload = 0xa5;
    if (address != port) { return false; }
    value = payload;
    return true;
  }
};

void external() {
  Device device;
  cpu12::Io peripherals{.owner = &device, .read = &Device::read};
  cpu12::Core core(cpu12::Memory::external(), &peripherals);
  constexpr auto instruction =
      cpu12::isa::program(cpu12::isa::opcode<cpu12::isa::Opcode::ldaa_extended>(),
                          std::array<cpu12::Byte, 2>{0xff, 0x90});
  const auto result = core.run_instruction(instruction);
  constexpr std::uint16_t wanted = 0xa500;
  require(result.retired == 1 && core.registers().d == wanted && device.reads == 1,
          "Explicit external memory uses the real supplied peripheral interface");
}

void validation() {
  bool empty = false;
  try {
    static_cast<void>(cpu12::Memory::unified({}));
  } catch (const std::invalid_argument &) { empty = true; }
  require(empty, "An empty unified memory request cannot silently select an external bus");
}
}

int main() {
  try {
    live_state();
    unified();
    retained_code();
    external();
    validation();
    std::cout << "Memory/Core/Io API, unified storage, retained native code and explicit external "
                 "access passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
