// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"
#include "isa.hpp"
#include "preparation_indexed.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cpu12::test::prepared_indexed {
namespace {
constexpr Address target = 0x2300, payload = 0x80ff;
constexpr Byte byte_payload = 0x80;
enum class Reject : std::uint8_t { none, pointer, operand };
enum class Yield : std::uint8_t { none, pointer, operand };

struct Device {
  Core *core = nullptr;
  State before;
  Progress progress;
  Address pointer = 0, written = 0;
  Reject reject = Reject::none;
  Yield yield = Yield::none;
  unsigned calls = 0;
  bool observed = true;

  bool access(const Address address) noexcept {
    ++calls;
    observed = observed && core->state() == before && core->progress() == progress;
    const bool indirect = address == pointer;
    observed = observed && (indirect ? calls == 1 : address == target && calls == 2);
    if ((indirect && yield == Yield::pointer) || (!indirect && yield == Yield::operand)) {
      core->request_yield();
    }
    return indirect ? reject != Reject::pointer : reject != Reject::operand;
  }

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    value = byte_payload;
    return static_cast<Device *>(owner)->access(address);
  }

  static bool read_word(void *owner, const Address address, Address &value) noexcept {
    auto &device = *static_cast<Device *>(owner);
    value = address == device.pointer ? target : payload;
    return device.access(address);
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &device = *static_cast<Device *>(owner);
    device.written = access.value;
    return device.access(access.address);
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    auto &device = *static_cast<Device *>(owner);
    device.written = access.value;
    return device.access(access.address);
  }
};

void rejected(Core &core, Device &device, const Operation operation, const RunResult result) {
  const bool pointer = device.reject == Reject::pointer;
  const bool write = operation.writes() && !pointer;
  const auto read_value = operation.word ? payload : byte_payload;
  auto value = write ? device.written : read_value;
  if (pointer) { value = target; }
  const RunResult expected{
      .fault = write ? Fault::memory_write : Fault::memory_read,
      .pc = entry,
      .opcode = operation.opcode,
      .address = pointer ? device.pointer : target,
      .value = static_cast<Address>(value),
      .access_size = static_cast<Byte>(pointer || operation.word ? 2 : 1),
      .read_mask = static_cast<Byte>(!write && !pointer && !operation.word ? byte_mask : 0)};
  auto state = device.before;
  state.failure = expected;
  require(result == expected && core.state() == state && core.progress() == device.progress &&
              device.calls == (pointer ? 1U : 2U),
          "Either rejected indirect transaction preserves the precise fault and complete original "
          "instruction");
  const auto calls = device.calls;
  require(core.run(1).retired == 0 && device.calls == calls,
          "A rejected pointer or operand transaction never replays");
}

void check(Core &core,
           Device &device,
           const Operation operation,
           const indexed::Form form,
           const bool timed) {
  constexpr Registers initial{
      .pc = entry, .d = 0x7fff, .x = 0xfff0, .y = 0x1234, .sp = 0x9000, .ccr = 0xff};
  core.set_registers(initial);
  device.before = core.state();
  device.progress = core.progress();
  device.pointer = indexed::pointer(form, initial);
  device.calls = 0;
  device.written = 0;
  constexpr unsigned large_grant = 100;
  const auto grant = device.yield == Yield::none ? 1U : large_grant;
  const auto result =
      timed ? core.run_until(device.progress.estimated_cycles + grant) : core.run(grant);
  require(
      device.observed,
      "Both indirect callbacks observe the original full state, PC, cycles and transaction order");
  if (device.reject != Reject::none) {
    rejected(core, device, operation, result);
    return;
  }
  auto wanted = operation.word
                    ? indexed::word_expected(initial, indexed::words[operation.index], payload)
                    : byte_expected(initial, operation, byte_payload);
  const auto length = 2 + form.extensions;
  wanted.pc = static_cast<Address>(entry + length);
  const RunResult expected{.retired = 1, .pc = entry, .opcode = operation.opcode};
  const auto cycles = operation.writes() ? 4U : 6U;
  require(result == expected && core.state() == State{.registers = wanted, .failure = {}} &&
              core.progress().estimated_cycles == device.progress.estimated_cycles + cycles &&
              device.calls == 2,
          "An accepted indirect operation completes atomically even when the pointer transaction "
          "requests yield");
  if (operation.writes()) {
    const auto value = operation.word ? initial.x : initial.d >> byte_bits;
    require(device.written == value,
            "Indirect writes retain the original architectural source across both callbacks");
  }
}

void form_cases(const indexed::Form form, const Operation operation) {
  const auto instruction = indexed::encode(form, operation.opcode);
  std::array<Byte, 6> bytes{};
  std::ranges::copy(instruction.view(), bytes.begin());
  bytes[instruction.length] = static_cast<Byte>(isa::Opcode::bra_relative);
  bytes[instruction.length + 1] = static_cast<Byte>(byte_mask - instruction.length - 1);
  const auto original = bytes;
  for (const auto reject : {Reject::none, Reject::pointer, Reject::operand}) {
    for (const auto yield : {Yield::none, Yield::pointer, Yield::operand}) {
      Device device{.before = {}, .progress = {}, .reject = reject, .yield = yield};
      Io io{.owner = &device,
            .read = Device::read,
            .write = Device::write,
            .read_word = Device::read_word,
            .write_word = Device::write_word};
      Core core(Memory::window({.first = entry,
                                .bytes = std::span(bytes).first(instruction.length + 2),
                                .writable = false}),
                &io);
      device.core = &core;
      constexpr std::array entries{entry};
      const auto report = core.prepare(entries);
      require(report.prepared_entries == 2 && report.unresolved_entries == 0 &&
                  report.native_bytes > 0 && report.failure.fault == Fault::none &&
                  device.calls == 0,
              "Indirect external operations prepare completely without either transaction");
      check(core, device, operation, form, false);
      check(core, device, operation, form, true);
      require(bytes == original, "Indirect preparation/execution never changes original code");
    }
  }
}
}

void boundaries() {
  const auto all = operations();
  constexpr std::array<std::size_t, 4> selected{0, 22, 20, 34};
  constexpr std::array forms{
      indexed::Form{.base = &Registers::x,
                    .displacement = 16,
                    .postbyte = 0xe3,
                    .extensions = 2,
                    .indirect = true},
      indexed::Form{
          .base = &Registers::x, .offset = indexed::Offset::d, .postbyte = 0xe7, .indirect = true}};
  for (const auto form : forms) {
    for (const auto index : selected) { form_cases(form, all[index]); }
  }
}
}
