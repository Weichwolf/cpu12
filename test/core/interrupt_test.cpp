// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "interrupt_model.hpp"
#include "observed_core.hpp"

#include <array>
#include <exception>
#include <iostream>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
using namespace cpu12::test::interrupts;
constexpr unsigned word_maximum = 0xffff, byte_maximum = 0xff;

void literal_frame(ObservedCore &core, Image &image) {
  put(image, seed.sp, literal);
  core.set_registers(seed);
  constexpr Registers wanted{
      .pc = 0xdef0, .d = 0x1234, .x = 0x5678, .y = 0x9abc, .sp = 5, .ccr = 0x97};
  accept(core, seed, wanted);
}

void stack_positions(ObservedCore &core, Image &image) {
  constexpr Byte before_guard = 0xa5;
  constexpr Byte after_guard = 0x5a;
  for (unsigned sp = 0; sp <= word_maximum; ++sp) {
    auto before = seed;
    before.sp = static_cast<Address>(sp);
    const auto lower = static_cast<Address>(sp + word_maximum);
    const auto upper = static_cast<Address>(sp + literal.size());
    image[lower] = before_guard;
    image[upper] = after_guard;
    put(image, before.sp, literal);
    core.set_registers(before);
    accept(core, before, expected(before, restored));
    require(image[lower] == before_guard && image[upper] == after_guard,
            "RTI is read-only at every stack position, including ffff wrapping");
    for (unsigned offset = 0; offset < literal.size(); ++offset) {
      require(image[static_cast<Address>(sp + offset)] == literal[offset],
              "RTI never rewrites saved interrupt context");
    }
  }
}

void word_values(ObservedCore &core, Image &image) {
  constexpr std::array fields{&Registers::pc, &Registers::d, &Registers::x, &Registers::y};
  for (const auto field : fields) {
    for (unsigned value = 0; value <= word_maximum; ++value) {
      auto target = restored;
      target.*field = static_cast<Address>(value);
      put(image, seed.sp, encode(target));
      core.set_registers(seed);
      accept(core, seed, expected(seed, target));
    }
  }
}

void condition_codes(ObservedCore &core, Image &image) {
  for (unsigned old = 0; old <= byte_maximum; ++old) {
    for (unsigned saved = 0; saved <= byte_maximum; ++saved) {
      auto before = seed;
      before.ccr = static_cast<Byte>(old);
      auto target = restored;
      target.ccr = static_cast<Byte>(saved);
      put(image, seed.sp, encode(target));
      core.set_registers(before);
      accept(core, before, expected(before, target));
    }
  }
}

void fetched_return() {
  Image image{};
  image[seed.pc] = rti[0];
  constexpr Byte nop = 0xa7;
  image[restored.pc] = nop;
  put(image, seed.sp, literal);
  ObservedCore core(Memory::unified(image));
  core.set_registers(seed);
  auto wanted = expected(seed, restored);
  ++wanted.pc;
  const auto result = core.run(2);
  require(result.retired == 2 && result.fault == Fault::none && core.registers() == wanted &&
              core.statistics().native_instructions == 2 && core.statistics().fetch_callbacks == 0,
          "Fetched RTI resumes at the saved address and executes the real next instruction");
  checked += 2;
}
}

int main() {
  try {
    Image image{};
    ObservedCore core(Memory::unified(image));
    literal_frame(core, image);
    stack_positions(core, image);
    word_values(core, image);
    condition_codes(core, image);
    require(core.statistics().native_instructions == checked &&
                core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
            "Full-RAM RTI uses direct native memory without an owner callback");
    fetched_return();
    bus_cases();
    std::cout
        << "Verified " << checked
        << " native RTI cases, every stack/word/CCR value, real bus order and precise faults.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
