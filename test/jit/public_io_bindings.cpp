// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "public_io.hpp"

namespace {
bool observe(cpu12::test::public_io::Bus &bus, cpu12::test::public_io::Access access) noexcept {
  if (bus.core != nullptr) {
    bus.observed_state = &bus.core->state();
    access.state = *bus.observed_state;
    access.state.registers = bus.core->registers();
    access.progress = bus.core->progress();
    if (bus.yield) { bus.core->request_yield(); }
  } else {
    if (bus.state != nullptr) { access.state = *bus.state; }
    if (bus.progress != nullptr) { access.progress = *bus.progress; }
  }
  if (bus.notify != nullptr) { bus.notify(bus.observer); }
  if (bus.calls >= bus.trace.size()) { return false; }
  bus.trace[bus.calls] = access;
  return bus.calls++ != bus.fail_at;
}

constexpr unsigned byte_bits = 8;
}

extern "C" bool
fixture_read(void *owner, const cpu12::Address address, cpu12::Byte &value) noexcept {
  if (owner == nullptr) { return false; }
  auto &bus = *static_cast<cpu12::test::public_io::Bus *>(owner);
  value = bus.memory[address];
  return observe(bus,
                 {.address = address, .value = value, .width = 1, .state = {}, .progress = {}});
}

extern "C" bool fixture_write(void *owner, const cpu12::ByteWrite access) noexcept {
  if (owner == nullptr) { return false; }
  auto &bus = *static_cast<cpu12::test::public_io::Bus *>(owner);
  bus.memory[access.address] = access.value;
  return observe(bus,
                 {.address = access.address,
                  .value = access.value,
                  .width = 1,
                  .write = true,
                  .state = {},
                  .progress = {}});
}

extern "C" bool
fixture_read_word(void *owner, const cpu12::Address address, cpu12::Address &value) noexcept {
  if (owner == nullptr) { return false; }
  auto &bus = *static_cast<cpu12::test::public_io::Bus *>(owner);
  value = static_cast<cpu12::Address>((unsigned{bus.memory[address]} << byte_bits) |
                                      bus.memory[static_cast<cpu12::Address>(address + 1U)]);
  return observe(bus,
                 {.address = address, .value = value, .width = 2, .state = {}, .progress = {}});
}

extern "C" bool fixture_write_word(void *owner, const cpu12::WordWrite access) noexcept {
  if (owner == nullptr) { return false; }
  auto &bus = *static_cast<cpu12::test::public_io::Bus *>(owner);
  bus.memory[access.address] = static_cast<cpu12::Byte>(access.value >> byte_bits);
  bus.memory[static_cast<cpu12::Address>(access.address + 1U)] =
      static_cast<cpu12::Byte>(access.value);
  return observe(bus,
                 {.address = access.address,
                  .value = access.value,
                  .width = 2,
                  .write = true,
                  .state = {},
                  .progress = {}});
}

extern "C" bool
fixture_read_masked(void *owner, const cpu12::MaskedByteRead access, cpu12::Byte &value) noexcept {
  if (owner == nullptr) { return false; }
  auto &bus = *static_cast<cpu12::test::public_io::Bus *>(owner);
  value = bus.memory[access.address];
  return observe(bus,
                 {.address = access.address,
                  .value = value,
                  .width = 1,
                  .mask = access.mask,
                  .state = {},
                  .progress = {}});
}
