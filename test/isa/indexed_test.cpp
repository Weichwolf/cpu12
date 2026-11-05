// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"
#include "reference.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace cpu12;
using namespace cpu12::test::indexed;
constexpr unsigned byte_bits = 8;
constexpr unsigned byte_mask = 0xff;
constexpr unsigned sign_bit = 0x80;
constexpr unsigned high_mask = 0xff00;
constexpr std::array<Registers, 3> seeds{
    {{.pc = 0xfffe, .d = 0x80ff, .x = 0, .y = 0x7fff, .sp = 0xffff, .ccr = 0xff},
     {.pc = 0, .d = 0x7f80, .x = 0xffff, .y = 0, .sp = 0x8000, .ccr = 0x21},
     {.pc = 0x7fff, .d = 0, .x = 0x8000, .y = 0xffff, .sp = 1, .ccr = 0x12}}};
constexpr std::array<unsigned, 8> operands{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff};
std::uint64_t checked = 0;

void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

std::uint16_t next(const std::uint16_t address) {
  return static_cast<std::uint16_t>(static_cast<unsigned>(address) + 1U);
}

void word(std::span<Byte> ram, const std::uint16_t address, const unsigned value) {
  ram[address] = static_cast<Byte>(value >> byte_bits);
  ram[next(address)] = static_cast<Byte>(value);
}

std::uint16_t prepare(std::span<Byte> ram, const Form form, const Registers &before) {
  const auto entry = pointer(form, before);
  if (!form.indirect) { return entry; }
  constexpr unsigned other_half = 0x8000;
  const auto target = static_cast<std::uint16_t>(entry ^ other_half);
  word(ram, entry, target);
  return target;
}

void retired(Core &core, const Encoding &instruction, Registers wanted, const Registers before) {
  wanted.pc = static_cast<std::uint16_t>(static_cast<unsigned>(before.pc) + instruction.length);
  core.set_registers(before);
  const auto actual = core.run_instruction(instruction.view());
  require(actual.retired == 1 && actual.fault == Fault::none && actual.pc == before.pc &&
              core.registers() == wanted,
          "Independent indexed address, operand, flags, PC and register expectation failed");
  ++checked;
}

void byte_arithmetic(Core &core, std::span<Byte> ram, const Form form, const Registers before) {
  constexpr unsigned indexed_delta = 0x20;
  constexpr std::array<unsigned, 16> immediate{0x86,
                                               0xc6,
                                               0x8b,
                                               0xcb,
                                               0x80,
                                               0xc0,
                                               0x81,
                                               0xc1,
                                               0x84,
                                               0xc4,
                                               0x8a,
                                               0xca,
                                               0x88,
                                               0xc8,
                                               0x85,
                                               0xc5};
  for (const auto operation : immediate) {
    const bool b = (operation & 0x40U) != 0;
    const auto left = b ? before.d & byte_mask : before.d >> byte_bits;
    for (const auto operand : operands) {
      const auto address = prepare(ram, form, before);
      const auto value = operand & byte_mask;
      ram[address] = static_cast<Byte>(value);
      const auto prediction = cpu12::test::expected(
          {.opcode = operation, .left = left, .right = value, .ccr = before.ccr});
      auto wanted = before;
      wanted.ccr = static_cast<std::uint8_t>(prediction.ccr);
      if (prediction.store) {
        wanted.d = static_cast<std::uint16_t>(b ? (before.d & high_mask) | prediction.value
                                                : (before.d & byte_mask) |
                                                      (prediction.value << byte_bits));
      }
      retired(core, encode(form, static_cast<Byte>(operation + indexed_delta)), wanted, before);
    }
  }
}

std::uint8_t store_flags(const Registers before, const unsigned value, const bool wide) {
  constexpr unsigned unchanged = 0xf1;
  constexpr unsigned word_sign = 0x8000;
  return static_cast<std::uint8_t>((before.ccr & unchanged) |
                                   ((value & (wide ? word_sign : sign_bit)) != 0 ? 8U : 0U) |
                                   (value == 0 ? 4U : 0U));
}

void byte_stores(Core &core, std::span<Byte> ram, const Form form, const Registers before) {
  constexpr unsigned store_a = 0x6a;
  for (const bool b : {false, true}) {
    const auto address = prepare(ram, form, before);
    const auto value = b ? before.d & byte_mask : before.d >> byte_bits;
    const auto left = static_cast<std::uint16_t>(static_cast<unsigned>(address) + 0xffffU);
    const auto right = next(address);
    const auto guard_left = ram[left];
    const auto guard_right = ram[right];
    auto wanted = before;
    wanted.ccr = store_flags(before, value, false);
    retired(core, encode(form, static_cast<Byte>(store_a + (b ? 1U : 0U))), wanted, before);
    require(ram[address] == value && ram[left] == guard_left && ram[right] == guard_right,
            "Indexed byte store changes exactly its addressed RAM byte");
  }
}

void word_operations(Core &core, std::span<Byte> ram, const Form form, const Registers before) {
  for (const auto operation : words) {
    for (const auto operand : operands) {
      const auto address = prepare(ram, form, before);
      word(ram, address, operand);
      const auto wanted = word_expected(before, operation, operand);
      const auto guard_address = next(next(address));
      const auto guard = ram[guard_address];
      retired(core, encode(form, operation.opcode), wanted, before);
      if (operation.function == Function::store) {
        const auto value = before.*operation.field;
        require(ram[address] == value >> byte_bits && ram[next(address)] == (value & byte_mask) &&
                    ram[guard_address] == guard,
                "Indexed word stores use big-endian values, wrap and original register sources");
      } else {
        require(ram[address] == operand >> byte_bits && ram[next(address)] == (operand & byte_mask),
                "Indexed word reads leave their operand unchanged");
      }
    }
  }
}

void matrix() {
  std::array<Byte, address_bytes> ram{};
  Core core(Memory::unified(ram));
  for (const auto form : forms()) {
    for (const auto before : seeds) {
      byte_arithmetic(core, ram, form, before);
      byte_stores(core, ram, form, before);
      word_operations(core, ram, form, before);
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
              core.statistics().native_instructions == checked,
          "Known dynamic byte/word/pointer RAM accesses stay entirely native");
}

struct Bus {
  std::uint16_t expected_address = 0, target = 0, payload = 0;
  unsigned reads = 0, writes = 0, word_reads = 0, word_writes = 0;
  bool reject_pointer = false, reject_operand = false;

  static bool read(void *owner, const std::uint16_t address, std::uint8_t &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.reads;
    value = static_cast<Byte>(bus.payload);
    return address == bus.expected_address && !bus.reject_operand;
  }

  static bool write(void *owner, const ByteWrite data) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.writes;
    bus.payload = data.value;
    return data.address == bus.expected_address && !bus.reject_operand;
  }

  static bool read_word(void *owner, const std::uint16_t address, std::uint16_t &value) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.word_reads;
    value = address == bus.target ? bus.expected_address : bus.payload;
    return address == bus.target ? !bus.reject_pointer
                                 : address == bus.expected_address && !bus.reject_operand;
  }

  static bool write_word(void *owner, const WordWrite data) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.word_writes;
    bus.payload = data.value;
    return data.address == bus.expected_address && !bus.reject_operand;
  }

  [[nodiscard]] Io io() noexcept {
    return {.owner = this,
            .read = read,
            .write = write,
            .read_word = read_word,
            .write_word = write_word};
  }
};

void dynamic_window() {
  constexpr std::uint16_t start = 0x100;
  constexpr std::uint16_t port = 0x110;
  constexpr std::uint8_t ram_value = 0x55;
  constexpr std::uint8_t bus_value = 0xaa;
  std::array<Byte, 16> ram{ram_value};
  Bus bus{.expected_address = port, .payload = bus_value};
  auto io = bus.io();
  Core core(Memory::window({.first = start, .bytes = ram}), &io);
  constexpr std::array<Byte, 2> load{0xa6, 0};
  core.set_registers({.x = start});
  require(core.run_instruction(load).retired == 1 && core.registers().d == ram_value << byte_bits &&
              bus.reads == 0,
          "Dynamic known RAM uses the native branch");
  core.set_registers({.x = port});
  require(core.run_instruction(load).retired == 1 && core.registers().d == bus_value << byte_bits &&
              bus.reads == 1 && core.statistics().compiled_entries == 1,
          "One cached address expression guards native RAM versus the actual external port");
  constexpr std::array<Byte, 2> word_load{0xec, 0};
  for (const auto seam : {std::uint16_t{0xff}, std::uint16_t{0x10f}}) {
    bus.expected_address = seam;
    constexpr std::uint16_t value = 0x9876;
    bus.payload = value;
    core.set_registers({.x = seam});
    require(core.run_instruction(word_load).retired == 1 && core.registers().d == value,
            "A partially known RAM word delegates its complete logical access");
  }
  require(bus.word_reads == 2 && bus.reads == 1,
          "Word seams do not silently split into byte callbacks or partial native accesses");
}

void indirect_bus() {
  constexpr std::uint16_t pointer_address = 0x200;
  constexpr std::uint16_t port = 0x110;
  constexpr std::uint16_t source = 0xabcd;
  Bus bus{.expected_address = port, .target = pointer_address};
  auto io = bus.io();
  Core core(Memory::external(), &io);
  constexpr std::array<Byte, 4> store{0x6a, 0xe3, 0, 0};
  constexpr std::array<Byte, 4> load{0xa6, 0xe3, 0, 0};
  const Registers before{.pc = 0xfffe, .d = source, .x = pointer_address, .ccr = 0xff};
  core.set_registers(before);
  auto wanted = before;
  wanted.pc = 2;
  wanted.ccr = store_flags(before, source >> byte_bits, false);
  require(core.run_instruction(store).retired == 1 && core.registers() == wanted &&
              bus.word_reads == 1 && bus.writes == 1 && bus.payload == source >> byte_bits,
          "An indirect store retains its original source across a real pointer-read ABI call");
  constexpr std::uint8_t received = 0x5a;
  bus.payload = received;
  core.set_registers(before);
  auto read_expected = before;
  read_expected.d = static_cast<std::uint16_t>((received << byte_bits) | (source & byte_mask));
  read_expected.pc = 2;
  read_expected.ccr = store_flags(before, received, false);
  require(core.run_instruction(load).retired == 1 && core.registers() == read_expected &&
              bus.reads == 1 && bus.word_reads == 2,
          "An indirect load uses the real pointer and operand responses in order");
  constexpr std::array<Byte, 4> word_store{0x6c, 0xe3, 0, 0};
  core.set_registers(before);
  require(core.run_instruction(word_store).retired == 1 && bus.word_writes == 1 &&
              bus.payload == source,
          "An indirect word store retains the complete source across the pointer ABI call");
  for (const bool pointer_failure : {true, false}) {
    bus.reject_pointer = pointer_failure;
    bus.reject_operand = !pointer_failure;
    core.set_registers(before);
    const auto actual = core.run_instruction(load);
    const auto count = bus.reads + bus.word_reads;
    require(actual.retired == 0 && actual.fault == Fault::memory_read && actual.pc == before.pc &&
                actual.address == (pointer_failure ? pointer_address : port) &&
                actual.access_size == (pointer_failure ? 2 : 1) && core.registers() == before,
            "Indirect pointer/operand failures retain exact phase, address, width and CPU state");
    require(core.run_instruction(load).retired == 0 && bus.reads + bus.word_reads == count,
            "A failed second indirect phase cannot replay an accepted pointer read");
  }
  bus.reject_pointer = false;
  core.set_registers(before);
  const auto failed = core.run_instruction(store);
  require(failed.retired == 0 && failed.fault == Fault::memory_write && failed.address == port &&
              failed.value == source >> byte_bits && bus.writes == 2 &&
              bus.payload == source >> byte_bits && core.registers() == before,
          "A refused indirect store retains actual external effects without CPU retirement");
}

void refusals() {
  Core core;
  const auto before = seeds[0];
  constexpr std::array<Byte, 3> lea{0x19, 0x1a, 0x1b};
  constexpr std::array<Byte, 8> indirect{0xe3, 0xeb, 0xf3, 0xfb, 0xe7, 0xef, 0xf7, 0xff};
  for (const auto opcode : lea) {
    for (const auto postbyte : indirect) {
      core.set_registers(before);
      const std::array<Byte, 4> instruction{opcode, postbyte, 0, 0};
      const auto length = (postbyte & 7U) == 3 ? std::size_t{4} : std::size_t{2};
      require(core.run_instruction(std::span(instruction).first(length)).fault ==
                      Fault::unsupported_instruction &&
                  core.registers() == before,
              "Illegal indirect LEA refuses without CPU or bus effects");
    }
  }
  constexpr std::array<Byte, 4> constant16{0xa6, 0xe2, 0x12, 0x34};
  for (const auto size : {std::size_t{1}, std::size_t{2}, std::size_t{3}}) {
    require(core.run_instruction(std::span(constant16).first(size)).retired == 0 &&
                core.registers() == before,
            "Incomplete indexed instruction bytes do not retire");
  }
  require(core.statistics().native_instructions == 0 && core.statistics().read_callbacks == 0 &&
              core.statistics().write_callbacks == 0,
          "Refusals produce no native or bus effects");
}
}

int main() {
  try {
    matrix();
    dynamic_window();
    indirect_bus();
    refusals();
    std::cout << "Verified " << checked
              << " independent indexed RAM/state cases plus dynamic "
                 "window, indirect bus, phase-fault and refusal contracts.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
