// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "preparation_memory.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address entry = 0x4000, direct_address = 0xfe, extended_address = 0x2000, stride = 8;
constexpr unsigned byte_bits = 8, byte_mask = 0xff, word_mask = 0xffff, direct_delta = 0x20;
constexpr std::array<Address, 8> values{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff};

struct Form {
  isa::Opcode opcode;
  bool word = false;
};

constexpr std::array forms{Form{.opcode = isa::Opcode::ldaa_extended},
                           Form{.opcode = isa::Opcode::ldab_extended},
                           Form{.opcode = isa::Opcode::adda_extended},
                           Form{.opcode = isa::Opcode::addb_extended},
                           Form{.opcode = isa::Opcode::adca_extended},
                           Form{.opcode = isa::Opcode::adcb_extended},
                           Form{.opcode = isa::Opcode::suba_extended},
                           Form{.opcode = isa::Opcode::subb_extended},
                           Form{.opcode = isa::Opcode::sbca_extended},
                           Form{.opcode = isa::Opcode::sbcb_extended},
                           Form{.opcode = isa::Opcode::cmpa_extended},
                           Form{.opcode = isa::Opcode::cmpb_extended},
                           Form{.opcode = isa::Opcode::anda_extended},
                           Form{.opcode = isa::Opcode::andb_extended},
                           Form{.opcode = isa::Opcode::oraa_extended},
                           Form{.opcode = isa::Opcode::orab_extended},
                           Form{.opcode = isa::Opcode::eora_extended},
                           Form{.opcode = isa::Opcode::eorb_extended},
                           Form{.opcode = isa::Opcode::bita_extended},
                           Form{.opcode = isa::Opcode::bitb_extended},
                           Form{.opcode = isa::Opcode::ldd_extended, .word = true},
                           Form{.opcode = isa::Opcode::ldx_extended, .word = true},
                           Form{.opcode = isa::Opcode::ldy_extended, .word = true},
                           Form{.opcode = isa::Opcode::lds_extended, .word = true},
                           Form{.opcode = isa::Opcode::addd_extended, .word = true},
                           Form{.opcode = isa::Opcode::subd_extended, .word = true},
                           Form{.opcode = isa::Opcode::cpd_extended, .word = true},
                           Form{.opcode = isa::Opcode::cpx_extended, .word = true},
                           Form{.opcode = isa::Opcode::cpy_extended, .word = true},
                           Form{.opcode = isa::Opcode::cps_extended, .word = true},
                           Form{.opcode = isa::Opcode::staa_extended},
                           Form{.opcode = isa::Opcode::stab_extended},
                           Form{.opcode = isa::Opcode::std_extended, .word = true},
                           Form{.opcode = isa::Opcode::stx_extended, .word = true},
                           Form{.opcode = isa::Opcode::sty_extended, .word = true},
                           Form{.opcode = isa::Opcode::sts_extended, .word = true}};

void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

struct Port {
  unsigned calls = 0;
  Address last_address = 0;

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    ++static_cast<Port *>(owner)->calls;
    static_cast<Port *>(owner)->last_address = address;
    value = 0;
    return false;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    ++static_cast<Port *>(owner)->calls;
    static_cast<Port *>(owner)->last_address = access.address;
    return false;
  }

  static bool read_word(void *owner, const Address address, Address &value) noexcept {
    ++static_cast<Port *>(owner)->calls;
    static_cast<Port *>(owner)->last_address = address;
    value = 0;
    return false;
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    ++static_cast<Port *>(owner)->calls;
    static_cast<Port *>(owner)->last_address = access.address;
    return false;
  }

  Io io() {
    return {.owner = this,
            .read = read,
            .write = write,
            .read_word = read_word,
            .write_word = write_word};
  }
};

struct Fixture {
  Fixture() : code(forms.size() * 2 * stride) {
    for (const auto form : forms) {
      for (const bool direct : {true, false}) {
        const auto offset = entries.size() * stride;
        entries.push_back(static_cast<Address>(entry + offset));
        const auto length = direct ? 2U : 3U;
        code[offset] =
            static_cast<Byte>(static_cast<unsigned>(form.opcode) - (direct ? direct_delta : 0U));
        code[offset + 1] =
            static_cast<Byte>(direct ? direct_address : extended_address >> byte_bits);
        if (!direct) { code[offset + 2] = static_cast<Byte>(extended_address); }
        code[offset + length] = static_cast<Byte>(isa::Opcode::bra_relative);
        code[offset + length + 1] = static_cast<Byte>(byte_mask - length - 1);
      }
    }
  }

  std::vector<Byte> code;
  std::vector<Address> entries;
};

struct Data {
  std::array<Byte, 4> direct{}, extended{}, direct_known{}, extended_known{};

  std::array<Ram, 3> regions(std::span<Byte> code, const bool tracked) {
    return {Ram{.first = entry, .bytes = code, .writable = false},
            Ram{.first = direct_address,
                .bytes = direct,
                .known = tracked ? std::span(direct_known) : std::span<Byte>{}},
            Ram{.first = extended_address,
                .bytes = extended,
                .known = tracked ? std::span(extended_known) : std::span<Byte>{}}};
  }

  void fill(const Address value) {
    constexpr Byte guard_high = 0xa5;
    constexpr Byte guard_low = 0x5a;
    direct = {
        static_cast<Byte>(value >> byte_bits), static_cast<Byte>(value), guard_high, guard_low};
    extended = direct;
    direct_known.fill(1);
    extended_known.fill(1);
  }

  bool operator==(const Data &) const = default;
};

Registers seed(const Address pc, const Address value, const Byte ccr) {
  constexpr Address x_bits = 0xa55a;
  constexpr Address y_bits = 0x5aa5;
  return {.pc = pc,
          .d = value,
          .x = static_cast<Address>(value ^ x_bits),
          .y = static_cast<Address>(value ^ y_bits),
          .sp = static_cast<Address>(word_mask - value),
          .ccr = ccr};
}

void prefixes(Core &prepared, Core &reference, Data &first, Data &second, const Registers state) {
  constexpr std::array<unsigned, 7> grants{0, 1, 2, 3, 4, 5, 17};
  for (const auto grant : grants) {
    first.fill(state.d);
    second = first;
    prepared.set_registers(state);
    reference.set_registers(state);
    require(prepared.run(grant) == reference.run(grant) && prepared.state() == reference.state() &&
                prepared.progress() == reference.progress() && first == second,
            "Mapped LLVM regions must preserve full state, memory and instruction prefixes");
    first.fill(state.d);
    second = first;
    prepared.set_registers(state);
    reference.set_registers(state);
    const auto deadline = prepared.progress().estimated_cycles + grant;
    require(prepared.run_until(deadline) == reference.run_until(deadline) &&
                prepared.state() == reference.state() &&
                prepared.progress() == reference.progress() && first == second,
            "Mapped LLVM regions preserve complete state and atomic cycle deadlines");
  }
}

std::size_t check_form(Core &prepared, Core &reference, Data &first, Data &second, Address pc) {
  std::size_t count = 0;
  for (const auto operand : values) {
    for (const auto value : values) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        first.fill(operand);
        second = first;
        const auto state = seed(pc, value, static_cast<Byte>(ccr));
        prepared.set_registers(state);
        reference.set_registers(state);
        require(prepared.run(1) == reference.run(1) && prepared.state() == reference.state() &&
                    prepared.progress() == reference.progress() && first == second,
                "Every mapped ALU/load/store must retain its full independent-native contract");
        ++count;
      }
      const auto state = seed(pc, value, byte_mask);
      prefixes(prepared, reference, first, second, state);
    }
  }
  return count;
}

std::size_t matrix(const bool tracked) {
  Fixture fixture;
  const auto original = fixture.code;
  Data first;
  Data second;
  auto first_regions = first.regions(fixture.code, tracked);
  auto second_regions = second.regions(fixture.code, tracked);
  Port port;
  auto io = port.io();
  Core prepared(Memory::mapped(first_regions), &io);
  Core reference(Memory::mapped(second_regions), &io);
  const auto report = prepared.prepare(fixture.entries);
  require(report.failure.fault == Fault::none && report.unresolved_entries == 0 &&
              report.prepared_entries == fixture.entries.size() * 2 && report.native_bytes > 0 &&
              prepared.progress() == Progress{} && port.calls == 0,
          "All direct/extended ALU/load/store forms must prepare fully without bus transactions");
  const auto compilations = prepared.statistics().compiled_entries;
  std::size_t count = 0;
  for (const auto pc : fixture.entries) {
    count += check_form(prepared, reference, first, second, pc);
  }
  require(fixture.code == original && prepared.statistics().compiled_entries == compilations &&
              port.calls == 0,
          "Known mapped RAM executes without callbacks, lazy compilation or source mutation");
  return count;
}
}

int main() try {
  const auto count = matrix(false) + matrix(true);
  cpu12::test::prepared_memory_boundaries();
  cpu12::test::prepared_memory_bindings();
  std::cout << count << " LLVM mapped-memory cases, prefixes and deadlines\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
