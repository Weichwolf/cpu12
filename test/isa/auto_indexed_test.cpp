// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"
#include "instruction_image.hpp"
#include "isa.hpp"
#include "reference.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using cpu12::test::InstructionCore;
using cpu12::test::WrappedRam;

namespace {
using namespace cpu12;
using namespace cpu12::test::indexed;
constexpr unsigned byte_bits = 8;
constexpr unsigned byte_mask = 0xff;
constexpr unsigned high_mask = 0xff00;
constexpr std::array<Field, 3> fields{&Registers::x, &Registers::y, &Registers::sp};
constexpr std::array lea_opcodes{
    op<isa::Opcode::leax_indexed>, op<isa::Opcode::leay_indexed>, op<isa::Opcode::leas_indexed>};
constexpr Address entry = 0x4000;
constexpr std::array<unsigned, 16> byte_opcodes{op<isa::Opcode::ldaa_immediate>,
                                                op<isa::Opcode::ldab_immediate>,
                                                op<isa::Opcode::adda_immediate>,
                                                op<isa::Opcode::addb_immediate>,
                                                op<isa::Opcode::suba_immediate>,
                                                op<isa::Opcode::subb_immediate>,
                                                op<isa::Opcode::cmpa_immediate>,
                                                op<isa::Opcode::cmpb_immediate>,
                                                op<isa::Opcode::anda_immediate>,
                                                op<isa::Opcode::andb_immediate>,
                                                op<isa::Opcode::oraa_immediate>,
                                                op<isa::Opcode::orab_immediate>,
                                                op<isa::Opcode::eora_immediate>,
                                                op<isa::Opcode::eorb_immediate>,
                                                op<isa::Opcode::bita_immediate>,
                                                op<isa::Opcode::bitb_immediate>};
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

void retired(InstructionCore &core,
             const std::span<const Byte> code,
             Registers wanted,
             const Registers before) {
  wanted.pc = static_cast<std::uint16_t>(static_cast<unsigned>(before.pc) + code.size());
  core.set_registers(before);
  core.select(code, before.pc);
  const auto actual = core.run(1);
  if (actual.retired != 1 || actual.fault != Fault::none || core.registers() != wanted) {
    throw std::runtime_error(
        "Auto/LEA opcode=" + std::to_string(code[0]) + " postbyte=" + std::to_string(code[1]) +
        " beforeX=" + std::to_string(before.x) + " beforeY=" + std::to_string(before.y) +
        " beforeSP=" + std::to_string(before.sp) + " wantedD=" + std::to_string(wanted.d) +
        " actualD=" + std::to_string(core.registers().d));
  }
  ++checked;
}

void byte_operations(InstructionCore &core,
                     std::span<Byte> ram,
                     const Auto point,
                     const Registers before) {
  constexpr unsigned indexed_delta = 0x20;
  for (const auto opcode : byte_opcodes) {
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
  constexpr unsigned first_store = op<isa::Opcode::staa_indexed>;
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

void word_operations(InstructionCore &core,
                     std::span<Byte> ram,
                     const Auto point,
                     const Registers before) {
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

void effective_addresses(InstructionCore &core, const Auto point, const Registers before) {
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

std::vector<std::vector<Byte>> auto_programs() {
  std::vector<std::vector<Byte>> programs;
  for (unsigned base = 0; base < fields.size(); ++base) {
    for (int change = -8; change <= 8; ++change) {
      if (change == 0) { continue; }
      for (const bool pre : {false, true}) {
        const Auto point{.base = base, .change = change, .pre = pre};
        const auto append = [&](const Byte opcode) {
          const auto code = point.code(opcode);
          programs.emplace_back(code.begin(), code.end());
        };
        for (const auto opcode : byte_opcodes) { append(static_cast<Byte>(opcode + 0x20U)); }
        append(op<isa::Opcode::staa_indexed>);
        append(op<isa::Opcode::stab_indexed>);
        for (const auto operation : words) { append(operation.opcode); }
        for (const auto opcode : lea_opcodes) { append(opcode); }
      }
    }
  }
  return programs;
}

void matrix() {
  std::array<Byte, address_bytes> ram{};
  const auto programs = auto_programs();
  constexpr std::array entries{entry};
  constexpr Address code_end = entry + 4;
  const std::array data{Ram{.bytes = std::span(ram).first(entry)},
                        Ram{.first = code_end, .bytes = std::span(ram).subspan(code_end)}};
  WrappedRam storage{.bytes = ram};
  Io io{.owner = &storage, .read_word = WrappedRam::read, .write_word = WrappedRam::write};
  InstructionCore core(programs, entries, &io, {}, data);
  for (unsigned base = 0; base < fields.size(); ++base) {
    for (int change = -8; change <= 8; ++change) {
      if (change == 0) { continue; }
      for (const bool pre : {false, true}) {
        const Auto point{.base = base, .change = change, .pre = pre};
        for (auto before : seeds) {
          // Keep every operand/base/flag input; real code is separate from varied data.
          before.pc = entry;
          byte_operations(core, ram, point, before);
          word_operations(core, ram, point, before);
          effective_addresses(core, point, before);
        }
      }
    }
  }
}

void lea_matrix() {
  std::vector<std::vector<Byte>> programs;
  for (const auto form : forms()) {
    if (form.indirect) { continue; }
    for (const auto opcode : lea_opcodes) {
      const auto code = encode(form, opcode);
      programs.emplace_back(code.view().begin(), code.view().end());
    }
  }
  constexpr std::array entries{seeds[0].pc, seeds[1].pc, seeds[2].pc};
  InstructionCore core(programs, entries);
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
}

struct Port {
  InstructionCore *core = nullptr;
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
  constexpr Byte store_x = op<isa::Opcode::stx_indexed>;
  constexpr Byte load_x = op<isa::Opcode::ldx_indexed>;
  constexpr std::uint16_t operand = 0x1234;
  const Registers before{
      .pc = 0xfffe, .d = 0xabcd, .x = 0x200, .y = 0x300, .sp = 0x400, .ccr = 0xff};
  for (const int change : {-8, -1, 1, 8}) {
    for (const bool pre : {false, true}) {
      const Auto point{.base = 0, .change = change, .pre = pre};
      Port port{.before = before, .address = point.address(before)};
      Io io{.owner = &port, .read_word = Port::read_word, .write_word = Port::write_word};
      const auto store = point.code(store_x);
      const auto load = point.code(load_x);
      const std::array programs{std::vector<Byte>(store.begin(), store.end()),
                                std::vector<Byte>(load.begin(), load.end())};
      const std::array positions{before.pc};
      InstructionCore core(programs, positions, &io);
      port.core = &core;
      core.set_registers(before);
      core.select(point.code(store_x), before.pc);
      const auto failure = core.run(1);
      require(failure.retired == 0 && failure.fault == Fault::memory_write &&
                  failure.address == port.address && failure.value == point.updated(before).x &&
                  failure.access_size == 2 && port.payload == point.updated(before).x &&
                  core.registers() == before,
              "Rejected auto stores retain actual adjusted-source effects and original CPU state");
      require(core.run(1).retired == 0 && port.writes == 1,
              "An auto-index store fault cannot silently repeat the write or index update");
      port.accept = true;
      auto wanted = word_expected(point.updated(before), words[12], 0);
      retired(core, point.code(store_x), wanted, before);
      require(port.writes == 2 && port.original_seen,
              "Callbacks see the explicit unretired CPU boundary, with actual adjusted data");
      port.accept = false;
      port.payload = operand;
      core.set_registers(before);
      core.select(point.code(load_x), before.pc);
      const auto refused = core.run(1);
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

}

int main() {
  try {
    matrix();
    faults();
    lea_matrix();
    std::cout << "Verified " << checked
              << " native auto-index/LEA cases, adjusted sources, "
                 "destination overlap, complete CCR preservation and atomic fault boundaries.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
