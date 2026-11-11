// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_frames.hpp"
#include "interrupt_model.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace cpu12::test::image_frames {
namespace {
namespace frames = cpu12::test::interrupts;
constexpr std::array<Address, 8> positions{0, 1, 4, 8, 9, 0xfffc, 0xffff, 0x9000};

void wait_case(Core &core, Fixture &fixture, const Form form, const Registers before) {
  const auto compiled = core.statistics().compiled_entries;
  core.set_registers(before);
  const auto cycles = core.progress().estimated_cycles;
  auto memory = fixture.bytes;
  constexpr unsigned stop_mask = 0x80;
  const bool masked = form.pc == stop && (before.ccr & stop_mask) != 0;
  auto wanted = sleeping(before);
  if (masked) {
    wanted.sp = before.sp;
  } else {
    auto saved = before;
    saved.pc = wanted.pc;
    frames::put(memory, wanted.sp, frames::encode(saved));
  }
  const auto execution = masked ? ExecutionState::running : form.execution;
  const auto cost = masked ? 2U : form.cycles;
  const auto result = core.step();
  if (core.registers() != wanted || core.progress().estimated_cycles != cycles + cost ||
      fixture.bytes != memory) {
    throw std::runtime_error(
        "Prepared wait PC=" + std::to_string(before.pc) + " SP=" + std::to_string(before.sp) +
        " CCR=" + std::to_string(before.ccr) + " retired=" + std::to_string(result.retired) +
        " actualPC=" + std::to_string(core.registers().pc) +
        " actualSP=" + std::to_string(core.registers().sp) +
        " cycles=" + std::to_string(core.progress().estimated_cycles - cycles) +
        " RAM=" + (fixture.bytes == memory ? "same" : "different"));
  }
  require(result.retired == 1 && result.fault == Fault::none && result.pc == form.pc &&
              result.opcode == form.opcode &&
              core.state() == State{.registers = wanted, .failure = {}, .execution = execution} &&
              core.progress().estimated_cycles == cycles + cost && fixture.bytes == memory &&
              core.statistics().compiled_entries == compiled,
          "Prepared WAI/STOP preserve independent full-CCR/wrapped-stack states, RAM and cycles");
  if (!masked) {
    require(core.run(16).retired == 0 && core.step().retired == 0 && fixture.bytes == memory &&
                core.progress().estimated_cycles == cycles + cost,
            "A prepared sleeping owner cannot fetch, retire or replay its frame");
  }
}

std::size_t waits(Core &core, Fixture &fixture) {
  std::size_t count = 0;
  for (const auto form : std::span(forms).first(2)) {
    for (const auto sp : positions) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        auto before = seed;
        before.pc = form.pc;
        before.sp = sp;
        before.ccr = static_cast<Byte>(ccr);
        wait_case(core, fixture, form, before);
        ++count;
      }
    }
  }
  return count;
}

std::size_t returns(Core &core, Fixture &fixture) {
  std::size_t count = 0;
  const auto compiled = core.statistics().compiled_entries;
  for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
    for (unsigned restored = 0; restored <= byte_mask; ++restored) {
      auto before = seed;
      before.pc = rti;
      before.sp = positions[(ccr + restored) % positions.size()];
      before.ccr = static_cast<Byte>(ccr);
      auto target = frames::restored;
      target.pc = entry + 11;
      target.ccr = static_cast<Byte>(restored);
      frames::put(fixture.bytes, before.sp, frames::encode(target));
      const auto memory = fixture.bytes;
      core.set_registers(before);
      const auto cycles = core.progress().estimated_cycles;
      const auto result = core.step();
      require(result.retired == 1 && result.fault == Fault::none && result.pc == rti &&
                  result.opcode == isa::rti[0] &&
                  core.state() == State{.registers = frames::expected(before, target),
                                        .failure = {},
                                        .returned_frame = true} &&
                  core.progress().estimated_cycles == cycles + 8 && fixture.bytes == memory &&
                  core.statistics().compiled_entries == compiled,
              "Prepared RTI stages all fields, enforces every sticky-X combination and opens its "
              "return window");
      require(core.run(1).retired == 1 && !core.state().returned_frame &&
                  core.progress().estimated_cycles == cycles + 9,
              "The first prepared instruction after RTI closes its exact return window");
      ++count;
    }
  }
  return count;
}

void events(Core &core, Fixture &fixture) {
  for (const auto request : requests) {
    core.set_registers(seed);
    fixture.bytes[vector] = static_cast<Byte>(rti >> byte_bits);
    fixture.bytes[vector + 1] = static_cast<Byte>(rti);
    const auto compiled = core.statistics().compiled_entries;
    const auto cycles = core.progress().estimated_cycles;
    require(core.run(16).retired == 1 && core.state().execution == ExecutionState::waiting &&
                core.interrupt(request).accepted && core.registers().pc == rti &&
                core.registers().sp == sleeping(seed).sp,
            "A prepared WAI consumes the actual declared IRQ without stacking a second frame");
    require(
        core.step().retired == 1 && core.state().returned_frame &&
            core.registers() == Registers{.pc = wai + 1,
                                          .d = seed.d,
                                          .x = seed.x,
                                          .y = seed.y,
                                          .sp = seed.sp,
                                          .ccr = seed.ccr} &&
            core.progress().estimated_cycles == cycles + 7 + 6 + 8 &&
            core.statistics().compiled_entries == compiled,
        "A complete prepared WAI/IRQ/RTI round trip restores the real frame without compilation");
  }
  auto before = seed;
  before.pc = stop;
  before.ccr |= 0x40;
  core.set_registers(before);
  const auto cycles = core.progress().estimated_cycles;
  require(core.run(16).retired == 1 && core.state().execution == ExecutionState::stopped,
          "Unmasked STOP ends a prepared grant at its actual sleep boundary");
  const auto memory = fixture.bytes;
  before.pc += 2;
  require(
      core.resume_from_stop().accepted &&
          core.state() == State{.registers = before, .failure = {}} && fixture.bytes == memory &&
          core.progress().estimated_cycles == cycles + 8,
      "Prepared masked-XIRQ STOP wake preserves RAM/time and restores only SP and execution state");
}
}
}

int main() {
  try {
    using namespace cpu12::test::image_frames;
    const auto image = compile(full);
    Fixture fixture(full);
    cpu12::Io io{
        .owner = &fixture, .read_word = Fixture::read_word, .write_word = Fixture::write_word};
    cpu12::Core core(fixture.memory(), &io);
    core.install(image, variant_id);
    const auto count = waits(core, fixture) + returns(core, fixture);
    events(core, fixture);
    deadlines(core, fixture);
    bus_cases();
    std::cout
        << "Verified " << count
        << " independent prepared frame states, exact IRQ/wake boundaries and real I/O faults.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
