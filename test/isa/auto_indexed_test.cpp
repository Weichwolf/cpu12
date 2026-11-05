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
constexpr unsigned high_mask = 0xff00;
constexpr std::array<Field, 3> fields{&Registers::x, &Registers::y, &Registers::sp};
constexpr std::array<Byte, 3> lea_opcodes{0x1a, 0x19, 0x1b};
constexpr std::array<unsigned, 8> operands{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff};
constexpr std::array<Registers, 3> seeds{
    {{.pc = 0xfffe, .d = 0x80ff, .x = 0, .y = 0x7fff, .sp = 0xffff, .ccr = 0xff},
     {.pc = 0, .d = 0x7f80, .x = 0xffff, .y = 0, .sp = 0x8000, .ccr = 0x21},
     {.pc = 0x7fff, .d = 0, .x = 0x8000, .y = 0xffff, .sp = 1, .ccr = 0x12}}};
std::uint64_t checked = 0;

void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

struct Auto {
  unsigned base;
  int change;
  bool pre;

  [[nodiscard]] std::uint16_t address(const Registers before) const {
    return static_cast<std::uint16_t>(static_cast<int>(before.*fields[base]) + (pre ? change : 0));
  }

  [[nodiscard]] Registers updated(Registers before) const {
    before.*fields[base] =
        static_cast<std::uint16_t>(static_cast<int>(before.*fields[base]) + change);
    return before;
  }

  [[nodiscard]] std::array<Byte, 2> code(const Byte opcode) const {
    const auto encoded =
        change > 0 ? static_cast<unsigned>(change - 1) : 16U - static_cast<unsigned>(-change);
    return {opcode, static_cast<Byte>((base << 6U) | 0x20U | (pre ? 0U : 0x10U) | encoded)};
  }
};

void word(std::span<Byte> ram, const std::uint16_t address, const unsigned value) {
  ram[address] = static_cast<Byte>(value >> byte_bits);
  ram[static_cast<std::uint16_t>(static_cast<unsigned>(address) + 1U)] = static_cast<Byte>(value);
}

void retired(Core &core,
             const std::span<const Byte> code,
             Registers wanted,
             const Registers before) {
  wanted.pc = static_cast<std::uint16_t>(static_cast<unsigned>(before.pc) + code.size());
  core.set_registers(before);
  const auto actual = core.run_instruction(code);
  if (actual.retired != 1 || actual.fault != Fault::none || core.registers() != wanted) {
    throw std::runtime_error(
        "Auto/LEA opcode=" + std::to_string(code[0]) + " postbyte=" + std::to_string(code[1]) +
        " beforeX=" + std::to_string(before.x) + " beforeY=" + std::to_string(before.y) +
        " beforeSP=" + std::to_string(before.sp) + " wantedD=" + std::to_string(wanted.d) +
        " actualD=" + std::to_string(core.registers().d));
  }
  ++checked;
}

void byte_operations(Core &core, std::span<Byte> ram, const Auto point, const Registers before) {
  constexpr std::array<unsigned, 16> operations{0x86,
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
  constexpr unsigned indexed_delta = 0x20;
  for (const auto opcode : operations) {
    for (const auto value : operands) {
      const bool b = (opcode & 0x40U) != 0;
      const auto left = b ? before.d & byte_mask : before.d >> byte_bits;
      const auto prediction = cpu12::test::expected(
          {.opcode = opcode, .left = left, .right = value & byte_mask, .ccr = before.ccr});
      ram[point.address(before)] = static_cast<Byte>(value);
      auto wanted = point.updated(before);
      wanted.ccr = static_cast<Byte>(prediction.ccr);
      if (prediction.store) {
        wanted.d = static_cast<std::uint16_t>(b ? (before.d & high_mask) | prediction.value
                                                : (before.d & byte_mask) |
                                                      (prediction.value << byte_bits));
      }
      retired(core, point.code(static_cast<Byte>(opcode + indexed_delta)), wanted, before);
    }
  }
  constexpr unsigned first_store = 0x6a;
  constexpr unsigned unchanged = 0xf1;
  constexpr unsigned sign = 0x80;
  for (const bool b : {false, true}) {
    const auto value = b ? before.d & byte_mask : before.d >> byte_bits;
    auto wanted = point.updated(before);
    wanted.ccr = static_cast<Byte>((before.ccr & unchanged) | ((value & sign) != 0 ? 8U : 0U) |
                                   (value == 0 ? 4U : 0U));
    retired(core, point.code(static_cast<Byte>(first_store + (b ? 1U : 0U))), wanted, before);
    require(ram[point.address(before)] == value, "Auto byte store uses the selected address");
  }
}

void word_operations(Core &core, std::span<Byte> ram, const Auto point, const Registers before) {
  const auto updated = point.updated(before);
  for (const auto operation : words) {
    for (const auto value : operands) {
      const auto address = point.address(before);
      const auto next = static_cast<std::uint16_t>(static_cast<unsigned>(address) + 1U);
      word(ram, address, value);
      const auto wanted = word_expected(updated, operation, value);
      retired(core, point.code(operation.opcode), wanted, before);
      if (operation.function == Function::store) {
        const auto stored = updated.*operation.field;
        require(ram[address] == stored >> byte_bits && ram[next] == (stored & byte_mask),
                "Auto word store reads its adjusted source, including base/source overlap");
      } else {
        require(ram[address] == value >> byte_bits && ram[next] == (value & byte_mask),
                "Auto load/compare/arithmetic leaves its memory operand unchanged");
      }
    }
  }
}

void effective_addresses(Core &core, const Auto point, const Registers before) {
  constexpr unsigned flag_values = 256;
  for (unsigned target = 0; target < fields.size(); ++target) {
    for (unsigned flags = 0; flags < flag_values; ++flags) {
      auto initial = before;
      initial.ccr = static_cast<Byte>(flags);
      auto wanted = point.updated(initial);
      wanted.*fields[target] = point.address(initial);
      retired(core, point.code(lea_opcodes[target]), wanted, initial);
    }
  }
}

void matrix() {
  std::array<Byte, address_bytes> ram{};
  Core core(Memory::unified(ram));
  for (unsigned base = 0; base < fields.size(); ++base) {
    for (int change = -8; change <= 8; ++change) {
      if (change == 0) { continue; }
      for (const bool pre : {false, true}) {
        const Auto point{.base = base, .change = change, .pre = pre};
        for (const auto before : seeds) {
          byte_operations(core, ram, point, before);
          word_operations(core, ram, point, before);
          effective_addresses(core, point, before);
        }
      }
    }
  }
  for (const auto form : forms()) {
    if (form.indirect) { continue; }
    for (const auto before : seeds) {
      for (unsigned target = 0; target < fields.size(); ++target) {
        auto wanted = before;
        wanted.*fields[target] = pointer(form, before);
        retired(core, encode(form, lea_opcodes[target]).view(), wanted, before);
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
              core.statistics().native_instructions == checked,
          "Auto RAM and LEA execution is native, without bus or ISA helpers");
}

struct Port {
  Core *core = nullptr;
  Registers before{};
  std::uint16_t address = 0, payload = 0;
  unsigned reads = 0, writes = 0;
  bool accept = false, original_seen = true;

  void observe(const std::uint16_t actual) noexcept {
    original_seen = original_seen && actual == address && core->registers() == before;
  }

  static bool read_word(void *owner, const std::uint16_t address, std::uint16_t &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.observe(address);
    ++port.reads;
    value = port.payload;
    return port.accept;
  }

  static bool write_word(void *owner, const WordWrite data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.observe(data.address);
    ++port.writes;
    port.payload = data.value;
    return port.accept;
  }
};

void faults() {
  constexpr Byte store_x = 0x6e;
  constexpr Byte load_x = 0xee;
  constexpr std::uint16_t operand = 0x1234;
  const Registers before{
      .pc = 0xfffe, .d = 0xabcd, .x = 0x200, .y = 0x300, .sp = 0x400, .ccr = 0xff};
  for (const int change : {-8, -1, 1, 8}) {
    for (const bool pre : {false, true}) {
      const Auto point{.base = 0, .change = change, .pre = pre};
      Port port{.before = before, .address = point.address(before)};
      Io io{.owner = &port, .read_word = Port::read_word, .write_word = Port::write_word};
      Core core(Memory::external(), &io);
      port.core = &core;
      core.set_registers(before);
      const auto failure = core.run_instruction(point.code(store_x));
      require(failure.retired == 0 && failure.fault == Fault::memory_write &&
                  failure.address == port.address && failure.value == point.updated(before).x &&
                  failure.access_size == 2 && port.payload == point.updated(before).x &&
                  core.registers() == before,
              "Rejected auto stores retain actual adjusted-source effects and original CPU state");
      require(core.run_instruction(point.code(store_x)).retired == 0 && port.writes == 1,
              "An auto-index store fault cannot silently repeat the write or index update");
      port.accept = true;
      auto wanted = word_expected(point.updated(before), words[12], 0);
      retired(core, point.code(store_x), wanted, before);
      require(port.writes == 2 && port.original_seen,
              "Callbacks see the explicit unretired CPU boundary, with actual adjusted data");
      port.accept = false;
      port.payload = operand;
      core.set_registers(before);
      const auto refused = core.run_instruction(point.code(load_x));
      require(refused.retired == 0 && refused.fault == Fault::memory_read &&
                  refused.address == port.address && refused.access_size == 2 &&
                  core.registers() == before && port.reads == 1,
              "Rejected auto loads change neither the index nor the destination");
      port.accept = true;
      wanted = word_expected(point.updated(before), words[2], operand);
      retired(core, point.code(load_x), wanted, before);
      require(port.reads == 2 && core.registers().x == operand && port.original_seen,
              "An accepted overlapping load replaces its index update after the real access");
    }
  }
}

void capture_keys() {
  std::array<Byte, address_bytes> ram{};
  Core core(Memory::unified(ram));

  struct Capture {
    std::array<Byte, 4> bytes;
    std::size_t length;
  };

  constexpr std::array valid{Capture{.bytes = {0xa7, 0, 0, 0}, .length = 1},
                             Capture{.bytes = {0x1a, 0, 0, 0}, .length = 2},
                             Capture{.bytes = {0xa6, 0xe0, 0, 0}, .length = 3},
                             Capture{.bytes = {0xa6, 0xe2, 0, 0}, .length = 4}};
  for (const auto input : valid) {
    const auto code = std::span(input.bytes);
    core.set_registers({});
    require(core.run_instruction(code.first(input.length)).retired == 1,
            "A complete zero-padded raw capture populates a real native entry");
    for (std::size_t length = 1; length <= input.bytes.size(); ++length) {
      if (length == input.length) { continue; }
      const auto before = core.registers();
      require(core.run_instruction(code.first(length)).retired == 0 && core.registers() == before,
              "Capture length distinguishes invalid prefixes and excess zeros from cached code");
    }
  }
  require(core.statistics().native_instructions == valid.size() &&
              core.statistics().compiled_entries == valid.size(),
          "Only complete valid captures compile and retire");
}
}

int main() {
  try {
    matrix();
    faults();
    capture_keys();
    std::cout << "Verified " << checked
              << " native auto-index/LEA cases, adjusted sources, "
                 "destination overlap, complete CCR preservation and atomic fault boundaries.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
