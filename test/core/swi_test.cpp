// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/isa.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {
using namespace cpu12;
using Image = std::array<Byte, address_bytes>;
constexpr Address vector = 0xfff6, handler = 0x4321;
constexpr Address stacked_sp = 0x3ff7;
constexpr Registers seed{
    .pc = 0x8000, .d = 0x1234, .x = 0x5678, .y = 0x9abc, .sp = 0x4000, .ccr = 0xff};

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

void set_vector(Image &image) {
  constexpr unsigned byte_bits = 8;
  image[vector] = static_cast<Byte>(handler >> byte_bits);
  image[vector + 1U] = static_cast<Byte>(handler);
}

void check_frame(const Image &image, const Registers before) {
  constexpr unsigned byte_bits = 8;
  const auto next = static_cast<Address>(before.pc + 1U);
  const std::array<Byte, 9> wanted{before.ccr,
                                   static_cast<Byte>(before.d),
                                   static_cast<Byte>(before.d >> byte_bits),
                                   static_cast<Byte>(before.x >> byte_bits),
                                   static_cast<Byte>(before.x),
                                   static_cast<Byte>(before.y >> byte_bits),
                                   static_cast<Byte>(before.y),
                                   static_cast<Byte>(next >> byte_bits),
                                   static_cast<Byte>(next)};
  for (unsigned index = 0; index < wanted.size(); ++index) {
    const auto at = static_cast<Address>(before.sp + address_bytes - 9U + index);
    require(image[at] == wanted[index], "SWI stores original CCR,B,A,X,Y and the next PC");
  }
}

void masks_and_return() {
  constexpr unsigned maximum_ccr = 0xff;
  constexpr Byte irq_mask = 0x10;
  constexpr std::array<Address, 3> positions{0, 1, 0xffff};
  for (const auto position : positions) {
    for (unsigned flags = 0; flags <= maximum_ccr; ++flags) {
      Image image{};
      set_vector(image);
      Core core(Memory::unified(image));
      auto before = seed;
      before.pc = position;
      before.sp = position;
      before.ccr = static_cast<Byte>(flags);
      core.set_registers(before);
      const auto result = core.run_instruction(isa::swi);
      auto wanted = before;
      wanted.pc = handler;
      wanted.sp = static_cast<Address>(before.sp - 9);
      wanted.ccr = static_cast<Byte>(before.ccr | irq_mask);
      require(result.retired == 1 && result.fault == Fault::none && result.pc == before.pc &&
                  core.registers() == wanted,
              "SWI ignores I/X masks, enters the actual vector and sets only I");
      check_frame(image, before);
      const auto returned = core.run_instruction(isa::rti);
      before.pc = static_cast<Address>(before.pc + 1U);
      require(returned.retired == 1 && returned.fault == Fault::none && core.registers() == before,
              "RTI restores complete SWI context and the wrapped next PC for every CCR");
    }
  }
}

struct Transfer {
  bool write = false;
  Address address = 0, value = 0;
  unsigned width = 2;
  bool operator==(const Transfer &) const = default;
};

constexpr std::array<Transfer, 6> sequence{
    {{.address = vector, .value = handler},
     {.write = true, .address = 0x3ffe, .value = 0x8001},
     {.write = true, .address = 0x3ffc, .value = 0x9abc},
     {.write = true, .address = 0x3ffa, .value = 0x5678},
     {.write = true, .address = 0x3ff8, .value = 0x3412},
     {.write = true, .address = 0x3ff7, .value = 0xff, .width = 1}}};

struct Bus {
  Image image{};
  Core *core = nullptr;
  unsigned calls = 0, refused = sequence.size();
  bool intact = true;

  bool observe(const Transfer transfer) noexcept {
    intact &= calls < sequence.size() && transfer == sequence[calls];
    intact &= core->registers() == seed;
    intact &= core->run_instruction(isa::nop).fault == Fault::reentrant;
    return calls++ != refused;
  }

  static bool read(void *owner, const Address address, Address &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    value = handler;
    return bus.observe({.address = address, .value = value});
  }

  static bool word(void *owner, const WordWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    if (!bus.observe({.write = true, .address = access.address, .value = access.value})) {
      return false;
    }
    constexpr unsigned byte_bits = 8;
    bus.image[access.address] = static_cast<Byte>(access.value >> byte_bits);
    bus.image[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value);
    return true;
  }

  static bool byte(void *owner, const ByteWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    if (!bus.observe(
            {.write = true, .address = access.address, .value = access.value, .width = 1})) {
      return false;
    }
    bus.image[access.address] = access.value;
    return true;
  }
};

void refusals() {
  for (unsigned refused = 0; refused <= sequence.size(); ++refused) {
    Bus bus{.refused = refused};
    Io io{.owner = &bus, .write = &Bus::byte, .read_word = &Bus::read, .write_word = &Bus::word};
    Core core(Memory::external(), &io);
    bus.core = &core;
    core.set_registers(seed);
    const auto result = core.run_instruction(isa::swi);
    if (refused == sequence.size()) {
      auto wanted = seed;
      wanted.pc = handler;
      wanted.sp = stacked_sp;
      require(result.retired == 1 && result.fault == Fault::none && core.registers() == wanted &&
                  bus.calls == sequence.size() && bus.intact,
              "External SWI commits exactly once after its real vector and complete frame succeed");
      check_frame(bus.image, seed);
      continue;
    }
    const auto failed = sequence[refused];
    require(
        result.retired == 0 && result.pc == seed.pc && result.opcode == isa::swi[0] &&
            result.fault == (refused == 0 ? Fault::memory_read : Fault::memory_write) &&
            result.address == failed.address && result.value == failed.value &&
            result.access_size == failed.width && bus.calls == refused + 1U && bus.intact &&
            core.registers() == seed,
        "Every SWI refusal preserves registers, reports exact access and performs no later I/O");
    Image accepted{};
    constexpr unsigned byte_bits = 8;
    for (unsigned index = 1; index < refused; ++index) {
      const auto transfer = sequence[index];
      accepted[transfer.address] = static_cast<Byte>(transfer.value >> byte_bits);
      accepted[transfer.address + 1U] = static_cast<Byte>(transfer.value);
    }
    require(bus.image == accepted, "SWI refusal retains exactly the writes already accepted");
    require(core.step() == result && core.run() == result && bus.calls == refused + 1U,
            "Latched SWI failures never replay vector reads or frame writes");
  }
}

void fetched() {
  Image image{};
  set_vector(image);
  image[seed.pc] = isa::swi[0];
  image[handler] = isa::rti[0];
  Core core(Memory::unified(image));
  core.set_registers(seed);
  const auto result = core.run(2);
  auto wanted = seed;
  ++wanted.pc;
  require(result.retired == 2 && result.fault == Fault::none && core.registers() == wanted,
          "Normal fetch/run executes native SWI and RTI without host interrupt substitution");
}
}

int main() {
  try {
    masks_and_return();
    refusals();
    fetched();
    std::cout << "SWI masks, wrapped next PC, independent frame, RTI and all I/O refusal phases "
                 "passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
