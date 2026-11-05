// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "wait_model.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>

namespace {
using namespace cpu12;
using namespace cpu12::test::waits;

void domains() {
  Image image{};
  Core core(Memory::unified(image));
  constexpr unsigned maximum = 0xffff;
  for (const auto form : forms) {
    for (unsigned position = 0; position <= maximum; ++position) {
      auto before = seed;
      before.sp = static_cast<Address>(position);
      const auto lower = static_cast<Address>(position + address_bytes - 10U);
      image[lower] = 1;
      image[before.sp] = 2;
      core.set_registers(before);
      enter(core, image, before, form);
      require(image[lower] == 1 && image[before.sp] == 2,
              "Wait pushes preserve both guards at every wrapped stack position");
      before = seed;
      before.pc = static_cast<Address>(position);
      core.set_registers(before);
      enter(core, image, before, form);
    }
  }
  require(core.statistics().native_instructions == entered &&
              core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "All stack/return-PC values execute natively without owner callbacks");
}

void flags() {
  Image image{};
  Core core(Memory::unified(image));
  constexpr unsigned maximum = 0xff;
  constexpr unsigned disabled = 0x80;
  for (const auto form : forms) {
    for (unsigned ccr = 0; ccr <= maximum; ++ccr) {
      auto before = seed;
      before.ccr = static_cast<Byte>(ccr);
      core.set_registers(before);
      if (form.state == ExecutionState::stopped && (ccr & disabled) != 0) {
        image[static_cast<Address>(before.sp - 1)] = 3;
        auto wanted = before;
        wanted.pc = static_cast<Address>(before.pc + 2U);
        const auto result = core.run_instruction(form.bytes());
        require(result.retired == 1 && result.fault == Fault::none &&
                    result.state == ExecutionState::running && core.registers() == wanted &&
                    image[static_cast<Address>(before.sp - 1)] == 3,
                "Disabled STOP advances two bytes without stacking, stopping or changing flags");
      } else {
        enter(core, image, before, form);
      }
    }
  }
}

void idle_boundaries() {
  Image image{};
  for (const auto form : forms) {
    Core core(Memory::unified(image));
    core.set_registers(seed);
    enter(core, image, seed, form);
    const auto before = core.registers();
    const auto statistics = core.statistics();
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
      const auto step = core.step();
      const auto run = core.run(std::numeric_limits<std::uint32_t>::max());
      const auto capture = core.run_instruction(nop);
      require(
          step.retired == 0 && step.fault == Fault::none && step.state == form.state &&
              run.retired == 0 && run.state == form.state && run.fault == Fault::none &&
              capture.retired == 0 && capture.state == form.state && capture.fault == Fault::none &&
              core.registers() == before,
          "Idle step, maximal run budget and captured inputs return promptly without execution");
    }
    require(core.run(0).state == form.state && core.state().execution == form.state &&
                core.statistics().native_instructions == statistics.native_instructions &&
                core.statistics().fetch_callbacks == 0,
            "Idle state survives cache invalidation and zero budgets with no hidden fetch");
  }
}

void connected_program() {
  for (const auto form : forms) {
    Image image{};
    constexpr Address reset_vector = 0xfffe;
    constexpr Address entry = 0x200;
    image[reset_vector] = 2;
    image[reset_vector + 1U] = 0;
    image[entry] = 0x10;
    constexpr Byte enable_masks = 0x2f;
    image[entry + 1U] = enable_masks;
    constexpr Address wait_pc = entry + 2;
    image[wait_pc] = form.code[0];
    if (form.length == 2) { image[wait_pc + 1U] = form.code[1]; }
    image[wait_pc + form.length] = nop[0];
    image[vector] = 4;
    image[vector + 1U] = 0;
    image[handler] = 8;
    image[handler + 1U] = rti[0];
    Core core(Memory::unified(image));
    core.set_registers(seed);
    require(core.reset(reset_vector).accepted,
            "Authored program begins with an actual reset vector");
    require(core.step().retired == 1,
            "Original CPU mask instruction enables STOP and IRQ after reset");
    const auto before = core.registers();
    const auto stopped = core.run(10);
    require(stopped.retired == 1 && stopped.state == form.state &&
                core.registers() == sleeping(before, form),
            "Fetched wait exits a larger instruction budget at its actual sleep boundary");
    check_frame(image, before, form);
    const auto frame = image;
    const auto stack = core.registers().sp;
    require(core.interrupt({.vector = vector}).accepted && core.registers().pc == handler &&
                core.registers().sp == stack && image == frame &&
                core.state().execution == ExecutionState::running,
            "Eligible interrupt reuses the real wait frame without any second stack write");
    require(core.step().retired == 1 && core.registers().x == static_cast<Address>(before.x + 1U),
            "Real native handler code executes after a genuine vector fetch");
    require(core.step().retired == 1 && core.registers().sp == before.sp &&
                core.registers().x == before.x && core.registers().pc == before.pc + form.length,
            "RTI restores the original context and returns immediately after the wait");
    require(core.step().retired == 1 && core.registers().pc == before.pc + form.length + 1U,
            "The actual following instruction executes after return");
  }
}

void cache_modes() {
  Image image{};
  image[vector] = 4;
  Core core(Memory::unified(image));
  core.set_registers(seed);
  require(core.interrupt({.vector = vector}).accepted &&
              core.registers().sp == static_cast<Address>(seed.sp + address_bytes - 9U) &&
              core.run_instruction(rti).retired == 1 && core.registers() == seed,
          "Ordinary entry creates and restores a genuine frame before warming the wake variant");
  enter(core, image, seed, forms[0]);
  const auto frame = image;
  const auto saved = core.registers();
  require(core.interrupt({.vector = vector}).accepted && core.registers().sp == saved.sp &&
              image == frame && core.run_instruction(rti).retired == 1 &&
              core.registers().sp == seed.sp && core.registers().pc == saved.pc,
          "The same vector has distinct warm native entries for ordinary stacking and frame reuse");
}

void masked_stop_wake() {
  Image image{};
  Core core(Memory::unified(image));
  constexpr unsigned x = 0x40;
  for (unsigned ccr = 0; ccr < x * 2; ++ccr) {
    auto before = seed;
    before.ccr = static_cast<Byte>(ccr);
    core.set_registers(before);
    enter(core, image, before, forms[1]);
    const auto saved = image;
    const auto result = core.resume_from_stop();
    const bool accepted = (ccr & x) != 0;
    auto wanted = accepted ? before : sleeping(before, forms[1]);
    if (accepted) { wanted.pc = static_cast<Address>(before.pc + 2U); }
    require(
        result.accepted == accepted && result.fault == Fault::none && core.registers() == wanted &&
            image == saved &&
            core.state().execution ==
                (accepted ? ExecutionState::running : ExecutionState::stopped),
        "Masked XIRQ STOP continuation adjusts only SP/state and uses no vector or frame reads");
  }
}

void wake_masks() {
  Image image{};
  image[vector] = 4;
  for (const auto form : forms) {
    Core core(Memory::unified(image));
    auto before = seed;
    constexpr Byte masked = 0x70;
    before.ccr = masked;
    core.set_registers(before);
    enter(core, image, before, form);
    const auto saved = core.state();
    const auto frame = image;
    const auto compiled = core.statistics().compiled_entries;
    require(!core.interrupt({.vector = vector}).accepted &&
                !core.interrupt({.vector = vector, .kind = InterruptKind::xirq}).accepted &&
                core.state() == saved && image == frame &&
                core.statistics().compiled_entries == compiled,
            "Masked requests neither service a vector nor change the genuine wait frame");
    constexpr Byte x_enabled = 0x30;
    before.ccr = x_enabled;
    core.set_registers(before);
    enter(core, image, before, form);
    const auto idle = core.registers();
    const auto x_frame = image;
    constexpr Byte servicing = 0x70;
    require(
        core.interrupt({.vector = vector, .kind = InterruptKind::xirq}).accepted &&
            core.registers().sp == idle.sp && core.registers().ccr == servicing &&
            core.state().execution == ExecutionState::running && image == x_frame &&
            core.run_instruction(rti).retired == 1 && core.registers().sp == before.sp &&
            core.registers().ccr == before.ccr && core.registers().pc == idle.pc,
        "Unmasked XIRQ wakes with I set, reuses the real frame and restores both masks via RTI");
  }
}
}

int main() {
  try {
    domains();
    flags();
    idle_boundaries();
    connected_program();
    masked_stop_wake();
    wake_masks();
    cache_modes();
    bus_cases();
    state_cases();
    std::cout << "Verified " << entered
              << " native wait entries, full stack/PC/CCR domains, genuine wake frames and CPU "
                 "state inspection.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
