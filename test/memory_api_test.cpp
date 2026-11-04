// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

void unified() {
  constexpr unsigned data_address = 16;
  constexpr std::uint16_t first_value = 0x1200;
  constexpr std::uint16_t second_value = 0x3400;
  constexpr std::uint8_t load_immediate = 0x86;
  constexpr std::uint8_t load_extended = 0xb6;
  constexpr std::uint8_t immediate_value = 0x7f;
  std::array<cpu12::Byte, 64> bytes{load_extended, 0, data_address};
  auto memory = cpu12::Memory::unified(bytes);
  require(memory.storage().bytes.data() == bytes.data() && memory.storage().first == 0,
          "Unified memory is a borrowed byte view without copying");
  cpu12::Core core(memory, nullptr, {.cache_entries = 1});
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
  bytes[0] = load_immediate;
  bytes[1] = immediate_value;
  core.set_registers({});
  result = core.run_instruction(memory.storage().bytes.first(2));
  require(result.retired == 1 &&
              core.registers().d == static_cast<std::uint16_t>(immediate_value << 8U) &&
              core.statistics().cache_resets == 1,
          "Actual changed captured bytes replace code within the configured cache bound");
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
  cpu12::Core core(cpu12::Memory::external(), &peripherals, {});
  constexpr std::array<std::uint8_t, 3> instruction{0xb6, 0xff, 0x90};
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
  for (const auto limit : {std::size_t{0}, std::size_t{4097}}) {
    bool rejected = false;
    try {
      const cpu12::Core invalid(cpu12::Memory::external(), nullptr, {.cache_entries = limit});
    } catch (const std::invalid_argument &) { rejected = true; }
    require(rejected, "Native code cache bounds must be explicitly valid");
  }
}
}

int main() {
  try {
    unified();
    external();
    validation();
    std::cout << "Memory/Core/Io/Options API, unified storage, cache bounds and explicit external "
                 "access passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
