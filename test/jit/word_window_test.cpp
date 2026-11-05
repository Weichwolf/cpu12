// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
using Field = Address Registers::*;
constexpr unsigned byte_bits = 8, byte_mask = 0xff;
constexpr Byte unchanged = 0xf1, negative = 8, zero = 4;
constexpr Byte postincrement_two = 0x31;
constexpr Registers seed{.pc = 0xfffe, .d = 0xa55a, .y = 0x8000, .sp = 0x7fff, .ccr = 0xff};
constexpr std::array<Byte, 6> backing{0xa5, 0, 0, 0xff, 0x80, 0x5a};
std::uint64_t checked = 0;

struct Operation {
  isa::Opcode opcode;
  Field field;
  bool write = false;
};

constexpr std::array operations{
    Operation{.opcode = isa::Opcode::ldd_indexed, .field = &Registers::d},
    Operation{.opcode = isa::Opcode::ldy_indexed, .field = &Registers::y},
    Operation{.opcode = isa::Opcode::ldx_indexed, .field = &Registers::x},
    Operation{.opcode = isa::Opcode::lds_indexed, .field = &Registers::sp},
    Operation{.opcode = isa::Opcode::std_indexed, .field = &Registers::d, .write = true},
    Operation{.opcode = isa::Opcode::sty_indexed, .field = &Registers::y, .write = true},
    Operation{.opcode = isa::Opcode::stx_indexed, .field = &Registers::x, .write = true},
    Operation{.opcode = isa::Opcode::sts_indexed, .field = &Registers::sp, .write = true}};

struct Layout {
  Address first;
  std::size_t size;
  bool writable = true;
  bool knowledge = false;
};

struct Storage {
  std::array<Byte, 6> bytes{};
  std::array<Byte, 6> known{};
};

struct Port {
  Core *core = nullptr;
  Registers before{};
  Address address = 0, value = 0;
  unsigned reads = 0, writes = 0;
  bool original_seen = true;

  static bool read(void *owner, const Address address, Address & /*value*/) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.address = address;
    ++port.reads;
    port.original_seen = port.original_seen && port.core->registers() == port.before;
    return false;
  }

  static bool write(void *owner, const WordWrite data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.address = data.address;
    port.value = data.value;
    ++port.writes;
    port.original_seen = port.original_seen && port.core->registers() == port.before;
    return false;
  }
};

struct Case {
  Layout layout;
  Operation operation;
  Address address;
  bool unknown = false;
  Byte ccr = seed.ccr;
};

bool admitted(const Case point, const Storage &storage) {
  if (point.address < point.layout.first ||
      unsigned{point.address} + 1U >= point.layout.first + point.layout.size) {
    return false;
  }
  if (point.operation.write) { return point.layout.writable; }
  if (!point.layout.knowledge) { return true; }
  const auto offset = point.address - point.layout.first + 1U;
  return storage.known[offset] != 0 && storage.known[offset + 1U] != 0;
}

void one(Core &core, Port &port, Storage &storage, const Case point) {
  storage.bytes = backing;
  storage.known.fill(0x80);
  if (point.unknown) { storage.known[2] = 0; }
  auto before = seed;
  before.x = point.address;
  before.ccr = point.ccr;
  port = {.core = &core, .before = before};
  auto wanted = before;
  wanted.x = static_cast<Address>(unsigned{point.address} + 2U);
  const auto source = wanted.*point.operation.field;
  const bool native = admitted(point, storage);
  const auto offset = native ? point.address - point.layout.first + 1U : 0U;
  auto expected = storage;
  unsigned value = source;
  if (native) {
    if (point.operation.write) {
      expected.bytes[offset] = static_cast<Byte>(source >> byte_bits);
      expected.bytes[offset + 1U] = static_cast<Byte>(source & byte_mask);
      if (point.layout.knowledge) {
        expected.known[offset] = 1;
        expected.known[offset + 1U] = 1;
      }
    } else {
      value = (unsigned{storage.bytes[offset]} << byte_bits) | storage.bytes[offset + 1U];
      wanted.*point.operation.field = static_cast<Address>(value);
    }
    wanted.pc = 0;
    wanted.ccr = static_cast<Byte>((before.ccr & unchanged) | (value >= 0x8000U ? negative : 0U) |
                                   (value == 0 ? zero : 0U));
  } else {
    wanted = before;
  }
  const std::array<Byte, 2> instruction{static_cast<Byte>(point.operation.opcode),
                                        postincrement_two};
  core.set_registers(before);
  const auto result = core.run_instruction(instruction);
  auto fault = point.operation.write ? Fault::memory_write : Fault::memory_read;
  if (native) { fault = Fault::none; }
  const unsigned callbacks = native ? 0 : 1;
  if (result.fault != fault || result.retired != (native ? 1U : 0U) || result.pc != before.pc ||
      core.registers() != wanted || storage.bytes != expected.bytes ||
      storage.known != expected.known || port.reads != (point.operation.write ? 0 : callbacks) ||
      port.writes != (point.operation.write ? callbacks : 0) || !port.original_seen ||
      (!native &&
       (result.address != point.address || result.access_size != 2 ||
        port.address != point.address || (point.operation.write && port.value != source)))) {
    throw std::runtime_error("Partial native word disagrees with independent RAM/state/owner");
  }
  ++checked;
}

void exercise(const Layout layout) {
  Storage storage;
  Port port;
  Io io{.owner = &port, .read_word = Port::read, .write_word = Port::write};
  Core core(
      Memory::window({.first = layout.first,
                      .bytes = std::span(storage.bytes).subspan(1, layout.size),
                      .known = layout.knowledge ? std::span(storage.known).subspan(1, layout.size)
                                                : std::span<Byte>{},
                      .writable = layout.writable}),
      &io);
  const auto end = static_cast<Address>(layout.first + layout.size);
  const std::array addresses{Address{0},
                             Address{1},
                             static_cast<Address>(unsigned{layout.first} + 0xffffU),
                             layout.first,
                             static_cast<Address>(unsigned{layout.first} + 1U),
                             static_cast<Address>(unsigned{end} + 0xfffeU),
                             static_cast<Address>(unsigned{end} + 0xffffU),
                             end,
                             Address{0xfffe},
                             Address{0xffff}};
  for (const auto operation : operations) {
    for (const auto address : addresses) {
      for (const bool unknown : {false, true}) {
        one(core,
            port,
            storage,
            {.layout = layout, .operation = operation, .address = address, .unknown = unknown});
      }
    }
  }
  for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
    for (const auto operation : operations) {
      one(core,
          port,
          storage,
          {.layout = layout,
           .operation = operation,
           .address = layout.first,
           .ccr = static_cast<Byte>(ccr)});
    }
  }
  if (layout.knowledge || !layout.writable) { return; }
  for (unsigned address = 0; address < address_bytes; ++address) {
    for (const auto operation : {operations.front(), operations[4]}) {
      one(core,
          port,
          storage,
          {.layout = layout, .operation = operation, .address = static_cast<Address>(address)});
    }
  }
}

void byte_flags() {
  Core core;
  for (const auto opcode : {isa::Opcode::ldaa_immediate, isa::Opcode::eora_immediate}) {
    for (unsigned value = 0; value <= byte_mask; ++value) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        auto before = seed;
        before.ccr = static_cast<Byte>(ccr);
        const auto result_byte =
            opcode == isa::Opcode::ldaa_immediate ? value : (seed.d >> byte_bits) ^ value;
        auto wanted = before;
        wanted.pc = 0;
        wanted.d = static_cast<Address>((result_byte << byte_bits) | (seed.d & byte_mask));
        wanted.ccr = static_cast<Byte>((ccr & unchanged) | (result_byte >= 0x80U ? negative : 0U) |
                                       (result_byte == 0 ? zero : 0U));
        core.set_registers(before);
        const std::array<Byte, 2> instruction{static_cast<Byte>(opcode), static_cast<Byte>(value)};
        const auto result = core.run_instruction(instruction);
        if (result.fault != Fault::none || result.retired != 1 || core.registers() != wanted) {
          throw std::runtime_error("Byte load/XOR disagrees with independent all-CCR NZV flags");
        }
        ++checked;
      }
    }
  }
}
}

int main() {
  try {
    constexpr std::array layouts{Layout{.first = 0, .size = 2},
                                 Layout{.first = 0x4000, .size = 3},
                                 Layout{.first = 0xfffc, .size = 4},
                                 Layout{.first = 0xffff, .size = 1},
                                 Layout{.first = 0x4000, .size = 3, .writable = false},
                                 Layout{.first = 0x4000, .size = 3, .knowledge = true}};
    for (const auto layout : layouts) { exercise(layout); }
    byte_flags();
    std::cout << checked
              << " byte/word NZV, boundaries, register aliases and refusal cases passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
