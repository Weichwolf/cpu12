// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>

namespace cpu12::test::resident {
inline constexpr Address entry = 0x0200, data = 0x4000;
inline constexpr Registers seed{.pc = entry, .d = 0xa55a, .x = 0x1234, .y = 0x5678, .sp = 0x9000};

inline void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Device {
  std::array<Byte, address_bytes> ram{};
  std::array<Ram, 3> regions;
  Core core;

  explicit Device(const std::span<const Byte> program)
      : regions{Ram{.first = 0, .bytes = std::span(ram).first(entry)},
                Ram{.first = entry,
                    .bytes = std::span(ram).subspan(entry, program.size()),
                    .writable = false},
                Ram{.first = static_cast<Address>(entry + program.size()),
                    .bytes = std::span(ram).subspan(entry + program.size())}},
        core(Memory::mapped(regions)) {
    std::ranges::copy(program, ram.begin() + entry);
  }

  void check(const Registers initial, const unsigned count, const Registers expected) {
    core.set_registers(initial);
    const auto result = core.run(count);
    require(result.fault == Fault::none && result.state == ExecutionState::running &&
                result.retired == count && core.registers() == expected,
            "Resident native region differs from independent register/flag expectations");
  }
};

}
