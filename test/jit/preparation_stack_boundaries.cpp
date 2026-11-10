// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "preparation_stack.hpp"
#include "stack_model.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace cpu12::test::prepared_stack {
namespace {
constexpr Address data = 0x9000, pointer = 0x2000, destination = 0x3000;
constexpr Registers initial{
    .pc = entry, .d = 0x7ffe, .x = pointer, .y = 0x5678, .sp = data, .ccr = 0xf0};
constexpr unsigned prefix_cycles = 2;
constexpr Byte addd = 0xc3, jsr = 0x15, indirect_x = 0xe3;
constexpr Address reloaded_x = 0xabcd;

struct Event {
  Address address = 0, value = 0;
  unsigned width = 0;
  bool write = false;
  bool operator==(const Event &) const = default;
};

struct Device {
  Core *core = nullptr;
  State expected{};
  Progress progress{};
  std::array<Byte, address_bytes> bytes{};
  std::array<Event, 2> events{};
  unsigned calls = 0;
  bool accept_read = false, accept_write = false, yield = false, observed = true;

  void observe(const Event event) noexcept {
    observed = observed && calls < events.size() && core->state() == expected &&
               core->progress() == progress;
    if (calls < events.size()) { events[calls] = event; }
    ++calls;
    if (yield) { core->request_yield(); }
  }

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    auto &self = *static_cast<Device *>(owner);
    value = self.bytes[address];
    self.observe({.address = address, .value = value, .width = 1});
    return self.accept_read;
  }

  static bool read_word(void *owner, const Address address, Address &value) noexcept {
    auto &self = *static_cast<Device *>(owner);
    value = stack::word(self.bytes, address);
    self.observe({.address = address, .value = value, .width = 2});
    return self.accept_read;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &self = *static_cast<Device *>(owner);
    self.bytes[access.address] = access.value;
    self.observe({.address = access.address, .value = access.value, .width = 1, .write = true});
    return self.accept_write;
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    auto &self = *static_cast<Device *>(owner);
    self.bytes[access.address] = static_cast<Byte>(access.value >> byte_bits);
    if (self.accept_write) {
      self.bytes[static_cast<Address>(access.address + 1U)] = static_cast<Byte>(access.value);
    }
    self.observe({.address = access.address, .value = access.value, .width = 2, .write = true});
    return self.accept_write;
  }

  [[nodiscard]] Io io() {
    return {.owner = this,
            .read = read,
            .write = write,
            .read_word = read_word,
            .write_word = write_word};
  }
};

void finish(Core &core,
            Device &device,
            const RunResult actual,
            const State wanted,
            const std::uint64_t progress,
            const unsigned calls,
            const std::uint64_t compilations) {
  require(core.state() == wanted && core.progress().estimated_cycles == progress &&
              device.calls == calls && device.observed &&
              core.statistics().compiled_entries == compilations,
          "Stack I/O must preserve exact state, timing, request order and prepared code");
  if (actual.fault != Fault::none) {
    require(core.run(1).retired == 0 && device.calls == calls,
            "A latched stack fault cannot replay an actual partial external effect");
  }
}

void external_stack(const stack::StackOp operation,
                    const bool accepted,
                    const bool yield,
                    const bool timed) {
  std::array<Byte, 6> code{addd, 0, 1, operation.opcode, bra, backedge};
  constexpr std::array<Address, 2> entries{entry, entry + 4};
  Device device{.accept_read = accepted, .accept_write = accepted, .yield = yield};
  auto io = device.io();
  Core core(Memory::window({.first = entry, .bytes = code, .writable = false}), &io);
  device.core = &core;
  const auto report = core.prepare(entries);
  require(report.prepared_entries == 3 &&
              report.unresolved_entries == (operation.target == stack::Target::pc ? 1U : 0U) &&
              report.failure.fault == Fault::none,
          "Every external stack fixture requires LLVM preparation");
  const auto compilations = core.statistics().compiled_entries;
  core.set_registers(initial);
  auto before = initial;
  before.pc = entry + 3;
  ++before.d;
  device.expected = {.registers = before, .failure = {}};
  device.progress = {.estimated_cycles = prefix_cycles};
  const Address operand = operation.width == 2 ? destination : 0;
  const auto changed = stack::expected(operation, before, operand);
  const auto address = operation.push ? changed.sp : before.sp;
  stack::word(device.bytes, address, operand);
  auto wanted = device.expected;
  const auto written = operation.push ? stack::value(before, operation.target) : operand;
  const RunResult failure{
      .fault = operation.push ? Fault::memory_write : Fault::memory_read,
      .pc = before.pc,
      .opcode = operation.opcode,
      .address = address,
      .value = written,
      .access_size = static_cast<Byte>(operation.width),
      .read_mask = static_cast<Byte>(!operation.push && operation.width == 1 ? byte_mask : 0)};
  const auto actual = timed ? core.run_until(prefix_cycles + 1) : core.run(2);
  auto expected = failure;
  expected.retired = 1;
  if (accepted) {
    wanted.registers = changed;
    expected = {.retired = 2, .pc = before.pc, .opcode = operation.opcode};
  } else {
    wanted.failure = failure;
  }
  require(actual == expected && device.events[0] == Event{.address = address,
                                                          .value = written,
                                                          .width = operation.width,
                                                          .write = operation.push},
          "External stack transaction and exact fault context must match the independent model");
  const auto pull_cycles = operation.target == stack::Target::pc ? 5U : 3U;
  const auto cycles = operation.push ? 2U : pull_cycles;
  finish(core, device, actual, wanted, prefix_cycles + (accepted ? cycles : 0), 1, compilations);
  if (operation.push) {
    require(device.bytes[address] == (operation.width == 2 ? written >> byte_bits : written),
            "Refused external writes retain their real high-byte effect");
  }
}

void external_call(const bool read, const bool write, const bool yield, const bool timed) {
  std::array<Byte, 9> code{addd, 0, 1, jsr, indirect_x, 0, 0, bra, backedge};
  constexpr std::array<Address, 1> entries{entry};
  Device device{.accept_read = read, .accept_write = write, .yield = yield};
  auto io = device.io();
  Core core(Memory::window({.first = entry, .bytes = code, .writable = false}), &io);
  device.core = &core;
  const auto report = core.prepare(entries);
  require(
      report.prepared_entries == 3 && report.unresolved_entries == 1 &&
          report.failure.fault == Fault::none,
      "Indirect call requires prepared LLVM code while retaining its dynamic target diagnostic");
  const auto compilations = core.statistics().compiled_entries;
  core.set_registers(initial);
  auto before = initial;
  before.pc = entry + 3;
  ++before.d;
  device.expected = {.registers = before, .failure = {}};
  device.progress = {.estimated_cycles = prefix_cycles};
  stack::word(device.bytes, pointer, destination);
  auto wanted = device.expected;
  constexpr Address stack_address = data - 2;
  constexpr Address return_pc = entry + 7;
  const RunResult failure{.fault = read ? Fault::memory_write : Fault::memory_read,
                          .pc = before.pc,
                          .opcode = code[3],
                          .address = read ? stack_address : pointer,
                          .value = read ? return_pc : destination,
                          .access_size = 2};
  auto expected = failure;
  expected.retired = 1;
  if (read && write) {
    wanted.registers.pc = destination;
    wanted.registers.sp = stack_address;
    expected = {.retired = 2, .pc = before.pc, .opcode = code[3]};
  } else {
    wanted.failure = failure;
  }
  const auto actual = timed ? core.run_until(prefix_cycles + 1) : core.run(2);
  require(
      actual == expected &&
          device.events[0] == Event{.address = pointer, .value = destination, .width = 2} &&
          (!read ||
           device.events[1] ==
               Event{.address = stack_address, .value = return_pc, .width = 2, .write = true}),
      "Indirect call reads its pointer before a precise accepted or partially refused stack write");
  finish(core,
         device,
         actual,
         wanted,
         prefix_cycles + (read && write ? 7U : 0U),
         read ? 2U : 1U,
         compilations);
  require(!read || device.bytes[stack_address] == return_pc >> byte_bits,
          "Indirect call refuses without rolling back the external high-byte store");
}

void rebind_stack() {
  const Program program{.code = {0x34, bra, byte_mask - 1}, .entries = {entry}, .nodes = 2};
  Fixture old(program);
  Fixture changed(program);
  Core core(Memory::mapped(old.grants));
  const auto original = old.bytes;
  const auto report = core.prepare(program.entries);
  const auto count = core.statistics().compiled_entries;
  core.set_memory(Memory::mapped(changed.grants));
  const auto reused = core.prepare(program.entries);
  auto before = initial;
  before.x = reloaded_x;
  core.set_registers(before);
  require(report.native_bytes != 0 && reused.prepared_entries == 2 && reused.native_bytes == 0 &&
              core.statistics().compiled_entries == count && core.run(1).retired == 1 &&
              stack::word(changed.bytes, data - 2) == before.x && old.bytes == original,
          "Reused stack IR binds the actual owner's RAM and never captures old memory pointers");
}

struct CcrInput {
  Byte value, old_x;
};

void deferred_pull(const bool accepted, const bool yield, const CcrInput point) {
  constexpr Byte andcc = 0x10;
  constexpr Byte clear_irq = 0xef;
  constexpr Byte pulc = 0x38;
  constexpr Byte irq = 0x10;
  std::array<Byte, 5> code{andcc, clear_irq, pulc, bra, backedge};
  constexpr std::array<Address, 1> entries{entry};
  Device device{.accept_read = accepted, .yield = yield};
  auto io = device.io();
  Core core(Memory::window({.first = entry, .bytes = code, .writable = false}), &io);
  device.core = &core;
  const auto report = core.prepare(entries);
  require(report.prepared_entries == 3 && report.unresolved_entries == 0 &&
              report.failure.fault == Fault::none,
          "CLI/PULC delay fixture requires complete LLVM preparation");
  const auto compilations = core.statistics().compiled_entries;
  auto before = initial;
  before.ccr = static_cast<Byte>(point.old_x | irq);
  core.set_registers(before);
  before.pc = entry + 2;
  before.ccr = point.old_x;
  device.expected = {.registers = before, .failure = {}, .irq_delay = 1};
  device.progress = {.estimated_cycles = 1};
  device.bytes[data] = point.value;
  require(core.run_until(1).retired == 1 && core.state() == device.expected &&
              !core.interrupt_ready(InterruptKind::maskable),
          "CLI defers IRQ eligibility until one complete prepared instruction retires");
  auto wanted = device.expected;
  const RunResult failure{.fault = Fault::memory_read,
                          .pc = before.pc,
                          .opcode = pulc,
                          .address = data,
                          .value = point.value,
                          .access_size = 1,
                          .read_mask = byte_mask};
  RunResult expected = failure;
  if (accepted) {
    constexpr stack::StackOp pull{
        .opcode = pulc, .target = stack::Target::ccr, .width = 1, .push = false};
    wanted.registers = stack::expected(pull, before, point.value);
    wanted.irq_delay = 0;
    expected = {.retired = 1, .pc = before.pc, .opcode = pulc};
  } else {
    wanted.failure = failure;
  }
  constexpr unsigned ample_deadline = 100;
  const auto actual = core.run_until(ample_deadline);
  require(actual == expected && core.interrupt_ready(InterruptKind::maskable) ==
                                    (accepted && (wanted.registers.ccr & irq) == 0),
          "Prepared PULC preserves sticky X, consumes existing delay only after success and "
          "returns at IRQ eligibility");
  finish(core, device, actual, wanted, 1 + (accepted ? 3U : 0U), 1, compilations);
}

void deferred_pulls() {
  constexpr std::array<Byte, 4> ccr_inputs{0, 0x10, 0x40, 0xff};
  for (const bool accepted : {false, true}) {
    for (const bool yield : {false, true}) {
      for (const auto input : ccr_inputs) {
        for (const Byte old_x : {Byte{0}, Byte{0x40}}) {
          deferred_pull(accepted, yield, {.value = input, .old_x = old_x});
        }
      }
    }
  }
}
}

void boundaries() {
  for (const auto operation : stack::operations) {
    for (const bool accepted : {false, true}) {
      for (const bool yield : {false, true}) {
        for (const bool timed : {false, true}) {
          external_stack(operation, accepted, yield, timed);
        }
      }
    }
  }
  for (const bool read : {false, true}) {
    for (const bool write : {false, true}) {
      for (const bool yield : {false, true}) {
        for (const bool timed : {false, true}) { external_call(read, write, yield, timed); }
      }
    }
  }
  rebind_stack();
  deferred_pulls();
}
}
