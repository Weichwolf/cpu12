// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
using Opcode = isa::Opcode;
constexpr Address entry = 0x0200, data = 0x4000, stack = 0x9000;

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Device {
  std::array<Byte, address_bytes> ram{};
  std::array<Ram, 3> grants;
  Core core;

  explicit Device(const std::span<const Byte> program, Io *io = nullptr)
      : grants{Ram{.bytes = std::span(ram).first(entry)},
               Ram{.first = entry,
                   .bytes = std::span(ram).subspan(entry, program.size()),
                   .writable = false},
               Ram{.first = static_cast<Address>(entry + program.size()),
                   .bytes = std::span(ram).subspan(entry + program.size())}},
        core(Memory::mapped(grants), io) {
    std::ranges::copy(program, ram.begin() + entry);
  }
};

void loops() {
  constexpr auto ordinary = isa::program(
      isa::decb, isa::opcode<Opcode::bne_relative>(), std::array<Byte, 1>{0xfd}, isa::wai);
  constexpr auto split = isa::program(isa::decb,
                                      isa::opcode<Opcode::beq_relative>(),
                                      std::array<Byte, 1>{2},
                                      isa::opcode<Opcode::bra_relative>(),
                                      std::array<Byte, 1>{0xfb},
                                      isa::wai);
  for (const unsigned count : {1U, 2U, 8U, 255U}) {
    for (const bool backedge : {false, true}) {
      const auto program =
          backedge ? std::span<const Byte>(split) : std::span<const Byte>(ordinary);
      Device device(program);
      const auto expected = backedge ? (5U * count) + 6U : (4U * count) + 5U;
      for (unsigned warm = 0; warm < 2; ++warm) {
        device.core.set_registers({.pc = entry, .d = static_cast<Address>(count), .sp = stack});
        const auto before = device.core.progress().estimated_cycles;
        const auto result = device.core.run();
        require(result.fault == Fault::none && result.state == ExecutionState::waiting &&
                    result.retired == (backedge ? 3U * count : (2U * count) + 1U) &&
                    device.core.progress().estimated_cycles - before == expected &&
                    device.core.registers().d == 0,
                "Whole resident loops count taken and final exits, including their WAI terminal");
      }
    }
  }
}

void region() {
  constexpr auto program = isa::program(isa::opcode<Opcode::ldd_extended>(),
                                        std::array<Byte, 2>{0x40, 0},
                                        isa::opcode<Opcode::cpd_immediate>(),
                                        std::array<Byte, 2>{0, 0},
                                        isa::opcode<Opcode::beq_relative>(),
                                        std::array<Byte, 1>{3},
                                        isa::opcode<Opcode::dex>(),
                                        isa::opcode<Opcode::bne_relative>(),
                                        std::array<Byte, 1>{0xf5},
                                        isa::wai);
  for (const Address count : {Address{1}, Address{8}, Address{1024}}) {
    Device device(program);
    device.ram[data + 1] = 1;
    device.core.set_registers({.pc = entry, .x = count, .sp = stack});
    const auto result = device.core.run();
    require(result.fault == Fault::none && result.state == ExecutionState::waiting &&
                result.retired == unsigned{count} * 5U + 1U &&
                device.core.progress().estimated_cycles == unsigned{count} * 10U + 5U &&
                device.core.registers().x == 0 && device.core.registers().d == 1,
            "A native two-phase region publishes exact timing without corrupting forwarded words");
  }
}

bool taken(const Opcode opcode, const Byte flags) {
  const auto condition = static_cast<unsigned>(opcode) & 15U;
  const bool c = (flags & 1U) != 0;
  const bool v = (flags & 2U) != 0;
  const bool z = (flags & 4U) != 0;
  const bool n = (flags & 8U) != 0;
  const std::array answers{true,
                           false,
                           !c && !z,
                           c || z,
                           !c,
                           c,
                           !z,
                           z,
                           !v,
                           v,
                           !n,
                           n,
                           n == v,
                           n != v,
                           !z && n == v,
                           z || n != v};
  return answers.at(condition);
}

void branches() {
  constexpr std::array forms{Opcode::bra_relative,
                             Opcode::brn_relative,
                             Opcode::bhi_relative,
                             Opcode::bls_relative,
                             Opcode::bcc_relative,
                             Opcode::bcs_relative,
                             Opcode::bne_relative,
                             Opcode::beq_relative,
                             Opcode::bvc_relative,
                             Opcode::bvs_relative,
                             Opcode::bpl_relative,
                             Opcode::bmi_relative,
                             Opcode::bge_relative,
                             Opcode::blt_relative,
                             Opcode::bgt_relative,
                             Opcode::ble_relative};
  for (const auto form : forms) {
    for (Byte flags = 0; flags < 16; ++flags) {
      for (const Byte displacement : {Byte{0}, Byte{7}}) {
        const std::array bytes{static_cast<Byte>(form), displacement};
        for (const bool block : {false, true}) {
          Device device(bytes);
          device.core.set_registers({.pc = entry, .ccr = flags});
          const auto result = block ? device.core.run(1) : device.core.step();
          const auto expected = taken(form, flags) ? 3U : 1U;
          require(result.retired == 1 && result.fault == Fault::none &&
                      device.core.progress().estimated_cycles == expected,
                  "All short branch predicates account for taken zero and nonzero displacement");
        }
      }
    }
  }
}

struct Owner {
  Core *core = nullptr;
  std::uint64_t before_write = 0;
  unsigned writes = 0;
  bool fail = false;

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &self = *static_cast<Owner *>(owner);
    ++self.writes;
    self.before_write = self.core->progress().estimated_cycles;
    self.core->request_yield();
    return !self.fail && access.address == data && access.value == 0;
  }
};

void owner_boundaries() {
  auto program = isa::program(isa::nop,
                              isa::nop,
                              isa::opcode<Opcode::staa_extended>(),
                              std::array<Byte, 2>{0x40, 0},
                              isa::nop);
  for (const bool fail : {false, true}) {
    Owner owner{.fail = fail};
    Io io{.owner = &owner, .write = Owner::write};
    Core core(Memory::window({.first = entry, .bytes = program, .writable = false}), &io);
    owner.core = &core;
    core.set_registers({.pc = entry});
    const auto result = core.run();
    require(owner.writes == 1 && owner.before_write == 2 && result.retired == (fail ? 2U : 3U) &&
                result.fault == (fail ? Fault::memory_write : Fault::none) &&
                core.progress().estimated_cycles == (fail ? 2U : 5U) &&
                core.registers().pc == entry + (fail ? 2U : 5U),
            "Real owner access sees prior progress; yield commits only a complete instruction");
  }
}

void deadlines() {
  constexpr auto loop =
      isa::program(isa::nop, isa::opcode<Opcode::bra_relative>(), std::array<Byte, 1>{0xfd});
  for (const unsigned deadline : {1U, 2U, 3U, 4U, 5U, 127U, 4096U}) {
    Device device(loop);
    device.core.set_registers({.pc = entry});
    const auto result = device.core.run_until(deadline);
    const auto cycles = device.core.progress().estimated_cycles;
    require(result.fault == Fault::none && result.retired != 0 && cycles >= deadline &&
                cycles < deadline + 3U,
            "A real time horizon admits complete native work with only final-instruction debt");
    require(device.core.run_until(cycles).retired == 0,
            "An already reached device horizon retires no fabricated instruction");
  }
}

void loop_deadlines() {
  constexpr auto program = isa::program(isa::opcode<Opcode::dex>(),
                                        isa::opcode<Opcode::bne_relative>(),
                                        std::array<Byte, 1>{0xfd},
                                        isa::wai);
  constexpr unsigned trips = 8;
  constexpr unsigned after_wait = 44;
  for (unsigned deadline = 0; deadline <= after_wait; ++deadline) {
    Device device(program);
    const Registers initial{.pc = entry, .x = trips, .sp = stack};
    device.core.set_registers(initial);
    auto wanted = initial;
    unsigned cycles = 0;
    unsigned retired = 0;
    bool waiting = false;
    while (cycles < deadline && !waiting) {
      if (wanted.pc == entry) {
        --wanted.x;
        wanted.ccr = wanted.x == 0 ? 4U : 0U;
        ++wanted.pc;
        ++cycles;
      } else if (wanted.pc == entry + 1) {
        wanted.pc = wanted.x == 0 ? entry + 3 : entry;
        cycles += wanted.x == 0 ? 1U : 3U;
      } else {
        ++wanted.pc;
        wanted.sp -= 9;
        cycles += 7;
        waiting = true;
      }
      ++retired;
    }
    const auto result = device.core.run_until(deadline);
    require(result.fault == Fault::none && result.retired == retired &&
                device.core.progress().estimated_cycles == cycles &&
                device.core.registers() == wanted &&
                result.state == (waiting ? ExecutionState::waiting : ExecutionState::running),
            "Native loop admission preserves every exact deadline through its true WAI exit");
  }
}

void interrupt_boundaries() {
  constexpr auto program = isa::program(
      isa::opcode<Opcode::andcc_immediate>(), std::array<Byte, 1>{0xef}, isa::nop, isa::nop);
  Device device(program);
  device.core.set_registers({.pc = entry, .ccr = 0x10});
  const auto clear = device.core.run_until(100);
  require(clear.retired == 1 && device.core.registers().pc == entry + 2 &&
              !device.core.interrupt_ready(InterruptKind::maskable),
          "Deadline execution returns on mask changes before the next required IRQ poll");
  const auto shadow = device.core.run_until(100);
  require(shadow.retired == 1 && device.core.registers().pc == entry + 3 &&
              device.core.interrupt_ready(InterruptKind::maskable),
          "Exactly one subsequent instruction expires the hardware interrupt deferral");
}

void event_progress() {
  constexpr auto program = isa::program(isa::wai, isa::rti);
  Device device(program);
  constexpr Address vector = 0xfff0;
  device.ram[vector] = static_cast<Byte>(entry >> 8U);
  device.ram[vector + 1] = 0;
  device.core.set_registers({.pc = entry, .sp = stack});
  constexpr unsigned irq_cycles = 9;
  constexpr unsigned return_cycles = 8;
  constexpr unsigned reuse_cycles = 6;
  constexpr unsigned wait_cycles = 7;
  constexpr unsigned reset_cycles = 5;
  unsigned expected = irq_cycles;
  require(device.core.interrupt({.vector = vector}).accepted &&
              device.core.progress().estimated_cycles == expected,
          "A real IRQ entry accounts for the documented nine nominal cycles");
  expected += return_cycles;
  require(device.core.run_instruction(isa::rti).retired == 1 &&
              device.core.progress().estimated_cycles == expected,
          "RTI accounts for eight nominal cycles");
  expected += reuse_cycles;
  require(device.core.interrupt({.vector = vector}).accepted &&
              device.core.progress().estimated_cycles == expected,
          "A tail-chained IRQ retains the existing frame and takes six nominal cycles");
  device.core.set_registers({.pc = entry, .sp = stack});
  expected += wait_cycles;
  require(device.core.step().state == ExecutionState::waiting &&
              device.core.progress().estimated_cycles == expected,
          "WAI accounts for its genuine frame before the owner supplies idle time");
  expected += reuse_cycles;
  require(device.core.interrupt({.vector = vector}).accepted &&
              device.core.progress().estimated_cycles == expected,
          "A pending interrupt wakes WAI using its already completed frame");
  expected += reset_cycles;
  require(device.core.reset(vector).accepted && device.core.progress().estimated_cycles == expected,
          "Accepted reset accounts for five nominal cycles without losing owner time");
}
}

int main() {
  try {
    loops();
    region();
    branches();
    owner_boundaries();
    deadlines();
    loop_deadlines();
    interrupt_boundaries();
    event_progress();
    std::cout << "Native nominal timing, event horizons, owner boundaries and IRQ polls passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
