// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "tail_chain_model.hpp"

#include <array>
#include <stdexcept>

namespace cpu12::test::tail_chains {
namespace {
struct Read {
  Address address;
  unsigned width;
  bool operator==(const Read &) const = default;
};

constexpr std::array<Read, 6> sequence{{{.address = frame_sp, .width = 1},
                                        {.address = frame_sp + 1, .width = 2},
                                        {.address = frame_sp + 3, .width = 2},
                                        {.address = frame_sp + 7, .width = 2},
                                        {.address = frame_sp + 5, .width = 2},
                                        {.address = vector, .width = 2}}};

struct Bus {
  Image image{};
  Core *core = nullptr;
  Registers expected = restored(initial);
  unsigned reads = 0, writes = 0;
  bool vector_allowed = false, writes_allowed = false, intact = true;
  std::array<Read, 8> trace{};

  bool observe(const Read read) noexcept {
    intact &=
        core->registers() == (reads < 5 ? initial : expected) && !core->interrupt_frame_available();
    bool refused = false;
    try {
      core->close_return_window();
    } catch (const std::logic_error &) { refused = true; }
    intact &= refused;
    if (reads < trace.size()) { trace[reads] = read; }
    ++reads;
    return read.address != vector || vector_allowed;
  }

  static bool byte(void *owner, const Address address, Byte &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    value = bus.image[address];
    return bus.observe({.address = address, .width = 1});
  }

  static bool word(void *owner, const Address address, Address &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    constexpr unsigned bits = 8;
    value = static_cast<Address>((static_cast<unsigned>(bus.image[address]) << bits) |
                                 bus.image[static_cast<Address>(address + 1U)]);
    return bus.observe({.address = address, .width = 2});
  }

  static bool write_byte(void *owner, const ByteWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    bus.intact &= bus.core->registers() == bus.expected && !bus.core->interrupt_frame_available();
    ++bus.writes;
    bus.image[access.address] = access.value;
    return bus.writes_allowed;
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    constexpr unsigned bits = 8;
    constexpr unsigned mask = 0xff;
    bus.intact &= bus.core->registers() == bus.expected && !bus.core->interrupt_frame_available();
    ++bus.writes;
    bus.image[access.address] = static_cast<Byte>(access.value >> bits);
    bus.image[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value & mask);
    return bus.writes_allowed;
  }
};

Io transport(Bus &bus) {
  return {.owner = &bus,
          .read = Bus::byte,
          .write = Bus::write_byte,
          .read_word = Bus::word,
          .write_word = Bus::write_word};
}

void failed_vector() {
  Bus bus;
  put_frame(bus.image, initial.sp);
  bus.image[vector] = 4;
  auto io = transport(bus);
  Core core(Memory::external(), &io);
  bus.core = &core;
  restore_frame(core, initial);
  const auto ready = core.state();
  const auto frame = bus.image;
  const auto result = core.interrupt({.vector = vector});
  require(!result.accepted && result.fault == Fault::memory_read &&
              result.pc == ready.registers.pc && result.address == vector &&
              result.value == target && result.access_size == 2 &&
              core.registers() == ready.registers && !core.interrupt_frame_available() &&
              core.state().returned_frame && bus.reads == 6 && bus.writes == 0 &&
              bus.image == frame && bus.intact,
          "A refused actual vector retains restored CPU/frame state, diagnostic target and read "
          "effects");
  const auto fault = core.state();
  bool refused = false;
  try {
    core.close_return_window();
  } catch (const std::logic_error &) { refused = true; }
  require(refused && core.state() == fault,
          "A board poll cannot discard a failed boundary or its diagnostic metadata");
  core.clear_code_cache();
  require(core.interrupt({.vector = vector}).fault == Fault::memory_read &&
              core.step().fault == Fault::memory_read && bus.reads == 6,
          "Fault latching blocks vector replay and code execution across cache invalidation");
  for (unsigned index = 0; index < sequence.size(); ++index) {
    require(bus.trace[index] == sequence[index],
            "External RTI reads CCR,B:A,X,PC,Y before the one pending vector read");
  }
}

void following_instruction() {
  Bus bus{.vector_allowed = true, .writes_allowed = true};
  put_frame(bus.image, initial.sp);
  bus.image[vector] = 4;
  auto io = transport(bus);
  Core core(Memory::external(), &io);
  bus.core = &core;
  restore_frame(core, initial);
  ++bus.expected.pc;
  require(core.run_instruction(nop).retired == 1 && !core.interrupt_frame_available(),
          "An actual following instruction closes the post-return reuse boundary");
  require(core.interrupt({.vector = vector}).accepted && bus.reads == 6 && bus.writes == 5 &&
              core.registers().sp == initial.sp && bus.intact,
          "After that boundary, normal interrupt entry performs all five actual stack writes");
  constexpr Byte restored_ccr = 0x20;
  constexpr Byte next_pc_low = 0x69;
  require(bus.image[initial.sp] == restored_ccr && bus.image[initial.sp + 8U] == next_pc_low,
          "Normal stacking records the cleared X bit and actual advanced PC instead of reusing old "
          "bytes");
}
}

void bus_cases() {
  failed_vector();
  following_instruction();
}
}
