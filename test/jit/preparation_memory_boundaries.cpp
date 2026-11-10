// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "preparation_memory.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>

namespace cpu12::test {
namespace {
constexpr Address entry = 0x4000, data_address = 0x2000, initial_d = 0x7ffe, prefix_d = 0x7fff;
constexpr Address input_value = 0x80ff;
constexpr Byte byte_value = 0x80, byte_mask = 0xff, preserved = 0xf1, upper = 0xf0;
constexpr unsigned byte_bits = 8;
constexpr std::array entries{entry};
constexpr std::array<Byte, 2> original_data{0x12, 0x34};

void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

struct Device {
  Core *core = nullptr;
  State expected_state;
  Progress expected_progress;
  unsigned calls = 0;
  bool accepted = false, yield = false, observed = true;
  Address written = 0;

  bool access(const Address address) noexcept {
    ++calls;
    observed = observed && address == data_address && core->state() == expected_state &&
               core->progress() == expected_progress;
    if (yield) { core->request_yield(); }
    return accepted;
  }

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    value = byte_value;
    return static_cast<Device *>(owner)->access(address);
  }

  static bool read_word(void *owner, const Address address, Address &value) noexcept {
    value = input_value;
    return static_cast<Device *>(owner)->access(address);
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

  Io io() {
    return {.owner = this,
            .read = read,
            .write = write,
            .read_word = read_word,
            .write_word = write_word};
  }
};

struct Operation {
  isa::Opcode opcode;
  bool word, write;
};

constexpr std::array operations{
    Operation{.opcode = isa::Opcode::ldaa_extended, .word = false, .write = false},
    Operation{.opcode = isa::Opcode::ldd_extended, .word = true, .write = false},
    Operation{.opcode = isa::Opcode::staa_extended, .word = false, .write = true},
    Operation{.opcode = isa::Opcode::std_extended, .word = true, .write = true}};

struct Fixture {
  explicit Fixture(const Operation operation)
      : code{static_cast<Byte>(isa::Opcode::addd_immediate),
             0,
             1,
             static_cast<Byte>(operation.opcode),
             static_cast<Byte>(data_address >> byte_bits),
             static_cast<Byte>(data_address),
             static_cast<Byte>(isa::Opcode::addd_immediate),
             0,
             2,
             static_cast<Byte>(isa::Opcode::bra_relative),
             backedge},
        regions{Ram{.first = entry, .bytes = code, .writable = false},
                Ram{.first = data_address,
                    .bytes = data,
                    .known = known,
                    .writable = !operation.write}} {}

  static constexpr Byte backedge = 0xf5;
  std::array<Byte, 11> code;
  std::array<Byte, 2> data = original_data, known{0, 0};
  std::array<Ram, 2> regions;
};

void rejected(Core &core, Device &device, const Operation operation, const RunResult result) {
  const auto output_value = operation.word ? prefix_d : prefix_d >> byte_bits;
  const auto calls = device.calls;
  const auto read_value = operation.word ? input_value : byte_value;
  const RunResult failure{
      .fault = operation.write ? Fault::memory_write : Fault::memory_read,
      .pc = entry + 3,
      .opcode = static_cast<Byte>(operation.opcode),
      .address = data_address,
      .value = static_cast<Address>(operation.write ? output_value : read_value),
      .access_size = static_cast<Byte>(operation.word ? 2 : 1),
      .read_mask = static_cast<Byte>(!operation.write && !operation.word ? byte_mask : 0)};
  auto returned = failure;
  returned.retired = 1;
  device.expected_state.failure = failure;
  require(result == returned && core.state() == device.expected_state &&
              core.progress() == device.expected_progress,
          "Rejected LLVM I/O retains the retired prefix, precise failing context and original "
          "current instruction");
  require(core.run(2).retired == 0 && device.calls == calls,
          "A latched LLVM bus fault never replays the rejected transaction");
}

void one_case(
    Core &core, Device &device, const bool timed, const Operation operation, const Byte ccr) {
  constexpr Address x = 0xa55a;
  constexpr Address y = 0x5aa5;
  constexpr Address sp = 0x9000;
  const Registers seed{.pc = entry, .d = initial_d, .x = x, .y = y, .sp = sp, .ccr = ccr};
  core.set_registers(seed);
  device.expected_state = {.registers = seed, .failure = {}};
  auto &registers = device.expected_state.registers;
  registers.pc = entry + 3;
  registers.d = prefix_d;
  registers.ccr = static_cast<Byte>(ccr & upper);
  const auto initial_progress = core.progress();
  device.expected_progress = {.estimated_cycles = initial_progress.estimated_cycles + 2};
  const auto calls = device.calls;
  constexpr unsigned extended_budget = 100;
  const auto budget = device.yield ? extended_budget : 3;
  const auto deadline = initial_progress.estimated_cycles + (device.yield ? extended_budget : 6);
  const auto result = timed ? core.run_until(deadline) : core.run(budget);
  require(device.calls == calls + 1 && device.observed,
          "LLVM I/O publishes exact complete prefix state and cycles before the real callback");
  const auto output_value = operation.word ? prefix_d : prefix_d >> byte_bits;
  if (operation.write) {
    require(device.written == output_value, "LLVM stores pass exact original byte/word values");
  }
  if (!device.accepted) {
    rejected(core, device, operation, result);
    return;
  }
  require(result.retired == (device.yield ? 2U : 3U) && result.fault == Fault::none,
          "Accepted LLVM I/O retires exactly once");
  registers.pc = entry + 6;
  if (!operation.write) {
    registers.d =
        operation.word ? input_value : static_cast<Address>((byte_value << byte_bits) | byte_mask);
  }
  registers.ccr = static_cast<Byte>((registers.ccr & preserved) | (operation.write ? 0U : 8U));
  device.expected_progress.estimated_cycles += 3;
  if (!device.yield) {
    registers.pc = entry + 9;
    registers.d = static_cast<Address>(registers.d + 2);
    registers.ccr = static_cast<Byte>((registers.ccr & upper) | 8U | (operation.write ? 2U : 0U));
    device.expected_progress.estimated_cycles += 2;
  }
  require(core.state() == device.expected_state && core.progress() == device.expected_progress,
          "Accepted LLVM memory operations preserve exact state, flags and cycles");
}

void io_boundaries() {
  for (const auto operation : operations) {
    Fixture fixture(operation);
    const auto source = fixture.code;
    for (const bool accepted : {false, true}) {
      for (const bool yield : {false, true}) {
        Device device{
            .expected_state = {}, .expected_progress = {}, .accepted = accepted, .yield = yield};
        auto io = device.io();
        Core core(Memory::mapped(fixture.regions), &io);
        device.core = &core;
        const auto report = core.prepare(entries);
        require(report.prepared_entries == 4 && report.unresolved_entries == 0 &&
                    report.failure.fault == Fault::none && report.native_bytes != 0 &&
                    device.calls == 0,
                "I/O regions compile fully without executing callbacks");
        for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
          one_case(core, device, false, operation, static_cast<Byte>(ccr));
          one_case(core, device, true, operation, static_cast<Byte>(ccr));
        }
        require(fixture.code == source && fixture.data == original_data &&
                    fixture.known == std::array<Byte, 2>{0, 0},
                "External transactions never silently initialize or overwrite mapped RAM");
      }
    }
  }
}
}

void prepared_memory_boundaries() {
  io_boundaries();
}
}
