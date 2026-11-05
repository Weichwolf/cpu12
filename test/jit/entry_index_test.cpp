// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr unsigned page_shift = 8, pages = 256, offset = 17;
constexpr Registers seed{.d = 0xa55a, .x = 0x1234, .y = 0x5678, .sp = 0x9000, .ccr = 0xff};

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

void execute(Core &core, const Address pc) {
  auto initial = seed;
  initial.pc = pc;
  core.set_registers(initial);
  const auto result = core.run(1);
  auto expected = initial;
  expected.pc = static_cast<Address>(pc + 1U);
  require(result.fault == Fault::none && result.retired == 1 &&
              result.state == ExecutionState::running && core.registers() == expected,
          "Sparse native-entry lookup must preserve the complete instruction result");
}

void index() {
  std::array<Byte, address_bytes> rom{};
  for (unsigned page = 0; page < pages; ++page) {
    rom[(page << page_shift) | offset] = isa::nop[0];
  }
  constexpr Address last = 0xffff;
  rom[last] = isa::nop[0];
  const Ram image{.bytes = std::span(rom), .writable = false};
  Core core(Memory::window(image));
  for (unsigned page = 0; page < pages; ++page) {
    execute(core, static_cast<Address>((page << page_shift) | offset));
  }
  execute(core, last);
  const auto compiled = core.statistics().compiled_entries;
  require(compiled == pages + 1U, "Every page and final address compile exactly one native entry");
  for (unsigned page = pages; page > 0; --page) {
    execute(core, static_cast<Address>(((page - 1U) << page_shift) | offset));
  }
  execute(core, last);
  require(core.statistics().compiled_entries == compiled,
          "All native entries remain retained across sparse-page lookups");
  auto other = image;
  other.identity = 1;
  core.set_memory(Memory::window(other));
  execute(core, offset);
  require(core.statistics().compiled_entries == compiled + 1,
          "A new binding identity selects a distinct native-entry index");
  core.set_memory(Memory::window(image));
  execute(core, offset);
  require(core.statistics().compiled_entries == compiled + 1,
          "Returning to a retained binding reuses its original indexed translation");
}
}

int main() {
  try {
    index();
    std::cout << "Sparse native entry pages, address wrap and binding reuse passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
