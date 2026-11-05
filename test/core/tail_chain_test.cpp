// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "tail_chain_model.hpp"

#include <array>
#include <exception>
#include <iostream>

namespace {
using namespace cpu12;
using namespace cpu12::test::tail_chains;

Address safe_vector(const Address stack) {
  const auto lower = static_cast<Address>(stack + address_bytes - 1U);
  const auto next = static_cast<Address>(vector + 1U);
  constexpr unsigned guarded_bytes = 11;
  constexpr Address alternate = 0x2000;
  return static_cast<Address>(vector - lower) < guarded_bytes ||
                 static_cast<Address>(next - lower) < guarded_bytes
             ? alternate
             : vector;
}

void stack_positions() {
  Image image{};
  Core core(Memory::unified(image));
  constexpr unsigned maximum = 0xffff;
  for (const auto kind : {InterruptKind::maskable, InterruptKind::xirq}) {
    for (unsigned sp = 0; sp <= maximum; ++sp) {
      auto before = initial;
      before.sp = static_cast<Address>(sp);
      put_frame(image, before.sp);
      const auto lower = static_cast<Address>(sp + address_bytes - 1U);
      const auto upper = static_cast<Address>(sp + literal.size());
      image[lower] = 1;
      image[upper] = 2;
      const auto selected = safe_vector(before.sp);
      image[selected] = 4;
      image[static_cast<Address>(selected + 1U)] = 0;
      restore_frame(core, before);
      accept(core, {.vector = selected, .kind = kind});
      for (unsigned index = 0; index < literal.size(); ++index) {
        require(image[static_cast<Address>(sp + index)] == literal[index],
                "Every returned stack frame remains byte-identical, including its saved but "
                "cleared X bit");
      }
      require(image[lower] == 1 && image[upper] == 2,
              "Tail entry preserves both stack guards at all16-bit stack positions");
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Known-RAM RTI and post-return entry execute without owner callbacks");
}

void flags() {
  Image image{};
  image[vector] = 4;
  Core core(Memory::unified(image));
  constexpr unsigned maximum = 0xff;
  for (const auto kind : {InterruptKind::maskable, InterruptKind::xirq}) {
    for (unsigned old = 0; old <= maximum; ++old) {
      for (unsigned stacked = 0; stacked <= maximum; ++stacked) {
        auto before = initial;
        before.ccr = static_cast<Byte>(old);
        put_frame(image, before.sp, static_cast<Byte>(stacked));
        restore_frame(core, before, static_cast<Byte>(stacked));
        const auto saved = core.state();
        constexpr unsigned x = 0x40;
        constexpr unsigned i = 0x10;
        const bool blocked =
            kind == InterruptKind::maskable ? (stacked & i) != 0 : (old & stacked & x) != 0;
        if (blocked) {
          const auto result = core.interrupt({.vector = vector, .kind = kind});
          require(!result.accepted && result.fault == Fault::none && core.state() == saved &&
                      core.interrupt_frame_available(),
                  "Masked requests retain the restored frame for another eligible source");
        } else {
          accept(core, {.vector = vector, .kind = kind});
        }
        require(image[before.sp] == stacked,
                "Post-RTI service never rewrites the saved CCR, even if X differs from restored "
                "CPU state");
      }
    }
  }
}

void vector_destinations() {
  Image image{};
  Core core(Memory::unified(image));
  constexpr unsigned maximum = 0xffff;
  constexpr unsigned bits = 8;
  constexpr unsigned mask = 0xff;
  for (unsigned destination = 0; destination <= maximum; ++destination) {
    put_frame(image, initial.sp);
    image[vector] = static_cast<Byte>(destination >> bits);
    image[vector + 1U] = static_cast<Byte>(destination & mask);
    restore_frame(core, initial);
    accept(core, {.vector = vector}, static_cast<Address>(destination));
  }
}

void native_program() {
  Image image{};
  constexpr Address reset = 0xfffe;
  constexpr Address second_vector = 0xfff0;
  constexpr Address second_handler = target + 0x10;
  constexpr Address entry = 0x200;
  constexpr std::array<Byte, 4> program{0x10, 0x2f, 0x3e, 0xa7};
  for (unsigned index = 0; index < program.size(); ++index) {
    image[entry + index] = program[index];
  }
  image[reset] = 2;
  image[reset + 1U] = 0;
  image[vector] = 4;
  image[second_vector] = 4;
  image[second_vector + 1U] = 0x10;
  image[target] = nop[0];
  image[target + 1U] = rti[0];
  image[second_handler] = nop[0];
  image[second_handler + 1U] = rti[0];
  Core core(Memory::unified(image));
  core.set_registers(initial);
  require(core.reset(reset).accepted && core.run(2).retired == 2 &&
              core.state().execution == ExecutionState::waiting,
          "Authored actual reset/ANDCC/WAI program reaches a genuine wait boundary");
  require(
      core.interrupt({.vector = vector}).accepted && core.run(2).retired == 2 &&
          core.registers().pc == entry + 3U && core.interrupt_frame_available(),
      "Real first handler code returns to the program before any following instruction executes");
  const auto frame = image;
  accept(core, {.vector = second_vector}, second_handler);
  require(image == frame && core.run(2).retired == 2 && core.registers().pc == entry + 3U &&
              core.registers().sp == initial.sp && core.interrupt_frame_available(),
          "A distinct pending vector runs its real handler through the unchanged original frame");
  require(core.run(1).retired == 1 && core.registers().pc == entry + 4U &&
              !core.interrupt_frame_available() && core.statistics().native_instructions == 7,
          "The actual next program instruction ends the return boundary after both handlers");
}
}

int main() {
  try {
    stack_positions();
    flags();
    vector_destinations();
    native_program();
    bus_cases();
    state_cases();
    std::cout << "Verified " << chained
              << " native post-RTI entries, unchanged saved frames, precise vector faults and "
                 "boundary state inspection.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
