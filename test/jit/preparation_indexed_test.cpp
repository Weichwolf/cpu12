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
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cpu12::test::prepared_indexed {
namespace {
struct Source {
  Operation operation;
  Form form;
  Address pc;
  std::size_t length;
};

struct Fixture {
  explicit Fixture(const std::span<const Operation> operations) {
    for (const auto operation : operations) {
      for (const auto form : forms()) {
        const auto instruction = indexed::encode(form.address, operation.opcode);
        const auto pc = static_cast<Address>(entry + (sources.size() * stride));
        sources.push_back(
            {.operation = operation, .form = form, .pc = pc, .length = instruction.length});
        entries.push_back(pc);
        std::ranges::copy(instruction.view(), bytes.begin() + pc);
        bytes[pc + instruction.length] = static_cast<Byte>(isa::Opcode::bra_relative);
        bytes[pc + instruction.length + 1] = static_cast<Byte>(byte_mask - instruction.length - 1);
      }
    }
    end = static_cast<Address>(entry + (sources.size() * stride));
    for (std::size_t address = 0; address < bytes.size(); ++address) {
      if (address < entry || address >= end) {
        bytes[address] = static_cast<Byte>((address * 13U) ^ (address >> byte_bits));
      }
    }
  }

  [[nodiscard]] bool writable(const Address address) const {
    return address < entry || address >= end;
  }

  [[nodiscard]] unsigned word(const Address address) const {
    return (static_cast<unsigned>(bytes[address]) << byte_bits) |
           bytes[static_cast<Address>(address + 1U)];
  }

  [[nodiscard]] bool mapped(const Address address, const Operation operation) const {
    if (operation.writes() && !writable(address)) { return false; }
    if (!operation.word) { return true; }
    const auto next = static_cast<Address>(address + 1U);
    if (operation.writes() && !writable(next)) { return false; }
    const auto region = [&](Address value) {
      if (value < entry) { return 0; }
      return value < end ? 1 : 2;
    };
    return region(address) == region(next);
  }

  std::array<Byte, address_bytes> bytes{};
  std::vector<Source> sources;
  std::vector<Address> entries;
  Address end = 0;
};

struct Bus {
  Fixture &fixture;
  Core *core = nullptr;
  State before;
  Progress progress;
  unsigned calls = 0;
  bool observed = true;

  void observe() noexcept {
    ++calls;
    observed = observed && core->state() == before && core->progress() == progress;
  }

  static bool read_word(void *owner, const Address address, Address &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    bus.observe();
    value = static_cast<Address>(bus.fixture.word(address));
    return true;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    bus.observe();
    if (!bus.fixture.writable(access.address)) { return false; }
    bus.fixture.bytes[access.address] = access.value;
    return true;
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    bus.observe();
    const auto next = static_cast<Address>(access.address + 1U);
    if (!bus.fixture.writable(access.address) || !bus.fixture.writable(next)) { return false; }
    bus.fixture.bytes[access.address] = static_cast<Byte>(access.value >> byte_bits);
    bus.fixture.bytes[next] = static_cast<Byte>(access.value);
    return true;
  }
};

struct Prediction {
  State state;
  RunResult result;
  std::uint64_t cycles = 0;
  unsigned calls = 0;
};

struct Store {
  Registers before, adjusted;
  Address address;
};

bool store(Fixture &fixture, Prediction &result, const Operation operation, const Store point) {
  const bool b = operation.opcode == static_cast<Byte>(isa::Opcode::stab_indexed);
  const auto byte_value = b ? point.adjusted.d & byte_mask : point.adjusted.d >> byte_bits;
  const auto value =
      operation.word ? point.adjusted.*indexed::words[operation.index].field : byte_value;
  const auto next = static_cast<Address>(point.address + 1U);
  if (!fixture.writable(point.address) || (operation.word && !fixture.writable(next))) {
    result.state.registers = point.before;
    result.result.retired = 0;
    result.result.fault = Fault::memory_write;
    result.result.address = point.address;
    result.result.value = static_cast<Address>(value);
    result.result.access_size = static_cast<Byte>(operation.word ? 2 : 1);
    result.state.failure = result.result;
    return false;
  }
  fixture.bytes[point.address] = static_cast<Byte>(operation.word ? value >> byte_bits : value);
  if (operation.word) { fixture.bytes[next] = static_cast<Byte>(value); }
  return true;
}

Prediction predict(Fixture &fixture, const Source source, const Registers before) {
  const auto operation = source.operation;
  const auto pointer = indexed::pointer(source.form.address, before);
  const auto address =
      source.form.address.indirect ? static_cast<Address>(fixture.word(pointer)) : pointer;
  auto adjusted = before;
  auto &base = adjusted.*source.form.address.base;
  base = static_cast<Address>(static_cast<int>(base) + source.form.update);
  const auto operand = operation.word ? fixture.word(address) : fixture.bytes[address];
  auto wanted = operation.word
                    ? indexed::word_expected(adjusted, indexed::words[operation.index], operand)
                    : byte_expected(adjusted, operation, operand);
  wanted.pc = static_cast<Address>(before.pc + source.length);
  Prediction result{.state = {.registers = wanted, .failure = {}},
                    .result = {.retired = 1, .pc = before.pc, .opcode = operation.opcode}};
  const Operation pointer_read{.opcode = 0, .word = true, .index = 0};
  if (source.form.address.indirect && !fixture.mapped(pointer, pointer_read)) { ++result.calls; }
  if (!fixture.mapped(address, operation)) { ++result.calls; }
  if (operation.writes() && !store(fixture,
                                   result,
                                   operation,
                                   {.before = before, .adjusted = adjusted, .address = address})) {
    return result;
  }
  if (source.form.address.indirect) {
    result.cycles = operation.writes() ? 4 : 6;
  } else if (operation.writes()) {
    result.cycles = source.form.address.extensions == 0 ? 2 : 3;
  } else {
    result.cycles = source.form.address.extensions == 2 ? 4 : 3;
  }
  return result;
}

auto regions(Fixture &fixture) {
  return std::array{
      Ram{.bytes = std::span(fixture.bytes).first(entry)},
      Ram{.first = entry,
          .bytes = std::span(fixture.bytes)
                       .subspan(entry, static_cast<std::size_t>(fixture.end - entry)),
          .writable = false},
      Ram{.first = fixture.end, .bytes = std::span(fixture.bytes).subspan(fixture.end)}};
}

void prefixes(Core &prepared, Fixture &actual, Bus &bus) {
  auto reference = actual;
  const auto grants = regions(reference);
  Bus port{.fixture = reference, .before = {}, .progress = {}};
  Io io{.owner = &port,
        .write = Bus::write,
        .read_word = Bus::read_word,
        .write_word = Bus::write_word};
  Core native(Memory::mapped(grants), &io);
  port.core = &native;
  constexpr std::array<unsigned, 7> budgets{0, 1, 2, 3, 4, 5, 17};
  constexpr std::array<Byte, 5> selected{0, 0x20, 0x30, 0xe3, 0xe7};
  for (const auto &source : actual.sources) {
    if (std::ranges::find(selected, source.form.address.postbyte) == selected.end()) { continue; }
    for (const auto budget : budgets) {
      for (const bool timed : {false, true}) {
        reference.bytes = actual.bytes;
        const Registers seed{
            .pc = source.pc, .d = 0x7fff, .x = 0xfff0, .y = 0x1234, .sp = 0x9000, .ccr = 0xff};
        prepared.set_registers(seed);
        native.set_registers(seed);
        const auto prepared_progress = prepared.progress().estimated_cycles;
        const auto native_progress = native.progress().estimated_cycles;
        const auto prepared_calls = bus.calls;
        const auto native_calls = port.calls;
        const auto result =
            timed ? prepared.run_until(prepared_progress + budget) : prepared.run(budget);
        const auto expected =
            timed ? native.run_until(native_progress + budget) : native.run(budget);
        require(result == expected && prepared.state() == native.state() &&
                    actual.bytes == reference.bytes &&
                    prepared.progress().estimated_cycles - prepared_progress ==
                        native.progress().estimated_cycles - native_progress &&
                    bus.calls - prepared_calls == port.calls - native_calls,
                "Indexed LLVM loops retain complete prefixes, atomic deadlines, memory and "
                "transaction counts");
      }
    }
  }
}

std::size_t batch(const std::span<const Operation> operations) {
  Fixture actual(operations);
  Fixture expected(operations);
  const auto grants = regions(actual);
  Bus bus{.fixture = actual, .before = {}, .progress = {}};
  Io io{.owner = &bus,
        .write = Bus::write,
        .read_word = Bus::read_word,
        .write_word = Bus::write_word};
  Core core(Memory::mapped(grants), &io);
  bus.core = &core;
  const auto report = core.prepare(actual.entries);
  require(report.prepared_entries == actual.entries.size() * 2 && report.unresolved_entries == 0 &&
              report.failure.fault == Fault::none && report.native_bytes != 0 && bus.calls == 0,
          "Every indexed ALU/load/store and established backedge must prepare completely");
  const auto compilations = core.statistics().compiled_entries;
  constexpr std::array<Registers, 3> seeds{
      Registers{.d = 0x80ff, .x = 0, .y = 0x7fff, .sp = 0xffff},
      Registers{.d = 0x7f80, .x = 0xffff, .y = 0, .sp = 0x8000},
      Registers{.d = 0, .x = 0x8000, .y = 0xffff, .sp = 1}};
  constexpr std::array<Byte, 8> flags{0, 1, 2, 4, 8, 0x20, 0xd0, 0xff};
  std::size_t cases = 0;
  for (const auto &source : actual.sources) {
    for (auto seed : seeds) {
      seed.pc = source.pc;
      for (const auto ccr : flags) {
        seed.ccr = ccr;
        core.set_registers(seed);
        bus.before = core.state();
        bus.progress = core.progress();
        const auto calls = bus.calls;
        const auto wanted = predict(expected, source, seed);
        const auto result = core.run(1);
        if (result != wanted.result || core.state() != wanted.state ||
            core.progress().estimated_cycles != bus.progress.estimated_cycles + wanted.cycles ||
            bus.calls != calls + wanted.calls || !bus.observed || actual.bytes != expected.bytes) {
          throw std::runtime_error(
              "Indexed LLVM mismatch opcode=" + std::to_string(source.operation.opcode) +
              " postbyte=" + std::to_string(source.form.address.postbyte) +
              " pc=" + std::to_string(seed.pc) + " CCR=" + std::to_string(ccr));
        }
        ++cases;
      }
    }
  }
  prefixes(core, actual, bus);
  require(core.statistics().compiled_entries == compilations,
          "Prepared indexed execution never lazily compiles any established instruction");
  return cases;
}
}

std::size_t matrix(const std::size_t part) {
  const auto all = operations();
  require(part < (all.size() + batch_size - 1) / batch_size, "Indexed code image must exist");
  const auto first = part * batch_size;
  return batch(std::span(all).subspan(first, std::min(batch_size, all.size() - first)));
}
}

int main(const int argc, const char *const argv[]) try {
  using cpu12::test::prepared_indexed::require;
  require(argc == 2, "Specify indexed code image 0, 1 or 2");
  const std::string_view argument(argv[1]);
  require(argument.size() == 1 && argument[0] >= '0' && argument[0] <= '2',
          "Specify indexed code image 0, 1 or 2");
  const auto part = static_cast<std::size_t>(argument[0] - '0');
  const auto count = cpu12::test::prepared_indexed::matrix(part);
  if (part == 0) { cpu12::test::prepared_indexed::boundaries(); }
  std::cout << count << " independent indexed LLVM full-state/memory/cycle cases\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
