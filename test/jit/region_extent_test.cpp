// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {
void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

void full_address_space() {
  using namespace cpu12;
  std::array<Byte, address_bytes> program{};
  program.fill(isa::nop[0]);
  Core core(Memory::window({.bytes = program, .writable = false}));
  constexpr Registers seed{.d = 0x1234, .x = 0x4567, .y = 0x89ab, .sp = 0xfffe, .ccr = 0xa5};
  core.set_registers(seed);
  constexpr auto count = static_cast<std::uint32_t>(address_bytes);
  const auto result = core.run(count);
  require(result.fault == Fault::none && result.retired == count &&
              result.state == ExecutionState::running && result.pc == count - 1 &&
              result.opcode == isa::nop[0] && core.registers() == seed &&
              core.statistics().compiled_entries == 1,
          "One complete immutable native region must cover the logical address wrap without caps");
  const auto warm = core.run(count);
  require(warm.fault == Fault::none && warm.retired == count && warm.pc == count - 1 &&
              warm.opcode == isa::nop[0] && core.registers() == seed &&
              core.statistics().compiled_entries == 1,
          "A large retained native region must reuse its code across the logical address wrap");
}
}

int main() {
  try {
    full_address_space();
    std::cout
        << "Whole-address-space native translation, long exits and warm budget reuse passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
