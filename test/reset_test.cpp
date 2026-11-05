// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "reset_model.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>

namespace {
using namespace cpu12;
using namespace cpu12::test::resetting;
constexpr unsigned word_maximum = 0xffff;

void vectors(Core &core, Image &image) {
  constexpr Byte high = 0x12;
  constexpr Byte low = 0x34;
  constexpr Address target = 0x1234;
  for (unsigned at = 0; at <= word_maximum; ++at) {
    const auto first = static_cast<Address>(at);
    const auto second = static_cast<Address>(at + 1);
    image[first] = high;
    image[second] = low;
    core.set_registers(seed);
    accept(core, first, seed, target);
    require(image[first] == high && image[second] == low,
            "Reset leaves vector storage intact at every address, including ffff wrapping");
  }
}

void destinations(Core &core, Image &image) {
  constexpr unsigned byte_bits = 8;
  constexpr unsigned byte_mask = 0xff;
  for (unsigned target = 0; target <= word_maximum; ++target) {
    image[vector] = static_cast<Byte>(target >> byte_bits);
    image[vector + 1U] = static_cast<Byte>(target & byte_mask);
    core.set_registers(seed);
    accept(core, vector, seed, static_cast<Address>(target));
  }
}

void flags(Core &core, Image &image) {
  constexpr Address target = 0xabcd;
  constexpr Byte high = 0xab;
  constexpr Byte low = 0xcd;
  image[vector] = high;
  image[vector + 1U] = low;
  constexpr unsigned maximum = 0xff;
  for (unsigned ccr = 0; ccr <= maximum; ++ccr) {
    auto before = seed;
    before.ccr = static_cast<Byte>(ccr);
    core.set_registers(before);
    accept(core, vector, before, target);
  }
}

void connected_program() {
  Image image{};
  constexpr Address entry = 0x200;
  constexpr std::array<Byte, 5> program{0x86, 0x7f, 0x8b, 1, 0xa7};
  std::ranges::copy(program, image.begin() + entry);
  image[vector] = 2;
  image[vector + 1U] = 0;
  Core core(Memory::unified(image));
  core.set_registers(seed);
  constexpr Registers wanted{.pc = entry + program.size(),
                             .d = 0x8034,
                             .x = seed.x,
                             .y = seed.y,
                             .sp = seed.sp,
                             .ccr = 0xfa};
  for (unsigned boot = 0; boot < 2; ++boot) {
    accept(core, vector, core.registers(), entry);
    const auto result = core.run(3);
    require(result.retired == 3 && result.fault == Fault::none && core.registers() == wanted,
            "A real reset vector leads into fetched native code, including after a warm reset");
  }
  require(core.statistics().native_instructions == 6 && core.statistics().compiled_entries == 3 &&
              core.statistics().read_callbacks == 0 && core.statistics().fetch_callbacks == 0,
          "Reset retires no instructions and retains valid byte-keyed native cache entries");
}
}

int main() {
  try {
    Image image{};
    Core core(Memory::unified(image));
    vectors(core, image);
    destinations(core, image);
    flags(core, image);
    require(core.statistics().native_instructions == 0 && core.statistics().read_callbacks == 0,
            "CPU reset control events do not fabricate instruction retirement or bus callbacks");
    connected_program();
    bus_cases();
    std::cout
        << "Verified " << accepted
        << " CPU reset events, all vector/destination/CCR values and precise bus boundaries.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
