// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr Address entry = 0x4000;
constexpr unsigned byte_bits = 8, sign_byte = 0x80, sign_word = 0x8000;
constexpr unsigned preserved = 0xf1, zero = 4, negative = 8;
constexpr unsigned high_byte = 0xff00;
enum class Target : std::uint8_t { a, b, d, x, y, sp };

struct Load {
  isa::Opcode opcode;
  Target target;
  bool word;
};

void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

Registers expected(Registers state, const Load operation, const Address operand) {
  switch (operation.target) {
    case Target::a:
      state.d = static_cast<Address>((state.d & std::numeric_limits<Byte>::max()) |
                                     (operand << byte_bits));
      break;
    case Target::b: state.d = static_cast<Address>((state.d & high_byte) | operand); break;
    case Target::d: state.d = operand; break;
    case Target::x: state.x = operand; break;
    case Target::y: state.y = operand; break;
    case Target::sp: state.sp = operand; break;
    default: throw std::logic_error("Invalid expected load target");
  }
  state.pc = static_cast<Address>(entry + (operation.word ? 3U : 2U));
  state.ccr =
      static_cast<Byte>((state.ccr & preserved) | (operand == 0 ? zero : 0) |
                        ((operand & (operation.word ? sign_word : sign_byte)) != 0 ? negative : 0));
  return state;
}

void compare_prefixes(Core &prepared, Core &reference, const Registers seed, const Address branch) {
  constexpr std::array<std::uint32_t, 5> grants{0, 1, 2, 3, 17};
  constexpr std::array<std::uint64_t, 5> deadlines{0, 1, 2, 3, 17};
  for (const auto pc : {entry, branch}) {
    auto state = seed;
    state.pc = pc;
    for (const auto grant : grants) {
      prepared.set_registers(state);
      reference.set_registers(state);
      require(prepared.run(grant) == reference.run(grant) &&
                  prepared.state() == reference.state() &&
                  prepared.progress() == reference.progress(),
              "Prepared loads must preserve complete instruction-prefix state and progress");
    }
    for (const auto delta : deadlines) {
      prepared.set_registers(state);
      reference.set_registers(state);
      const auto deadline = prepared.progress().estimated_cycles + delta;
      require(prepared.run_until(deadline) == reference.run_until(deadline) &&
                  prepared.state() == reference.state() &&
                  prepared.progress() == reference.progress(),
              "Prepared loads must preserve atomic cycle-deadline boundaries");
    }
  }
}

void check(const Load operation, Address operand) {
  const std::size_t length = operation.word ? 3U : 2U;
  const auto size = length + 2;
  if (!operation.word) { operand = static_cast<Byte>(operand); }
  std::array<Byte, 5> bytes{};
  bytes[0] = static_cast<Byte>(operation.opcode);
  bytes[1] = static_cast<Byte>(operation.word ? operand >> byte_bits : operand);
  if (operation.word) { bytes[2] = static_cast<Byte>(operand); }
  bytes[length] = static_cast<Byte>(isa::Opcode::bra_relative);
  bytes[length + 1] = static_cast<Byte>(std::numeric_limits<Byte>::max() - size + 1);
  const auto original = bytes;
  const auto memory =
      Memory::window({.first = entry, .bytes = std::span(bytes).first(size), .writable = false});
  Core prepared(memory);
  Core reference(memory);
  constexpr std::array entries{entry};
  const auto report = prepared.prepare(entries);
  require(report.failure.fault == Fault::none && report.prepared_entries == 2 &&
              report.unresolved_entries == 0 && report.native_bytes != 0,
          "Each immediate load and its backedge must install real LLVM entries");
  const auto statistics = prepared.statistics();
  for (unsigned ccr = 0; ccr <= std::numeric_limits<Byte>::max(); ++ccr) {
    const Registers seed{.pc = entry,
                         .d = 0xa55a,
                         .x = 0x1234,
                         .y = 0x5678,
                         .sp = 0x9000,
                         .ccr = static_cast<Byte>(ccr)};
    prepared.set_registers(seed);
    const auto result = prepared.run(1);
    require(result.fault == Fault::none && result.retired == 1 &&
                prepared.registers() == expected(seed, operation, operand),
            "Immediate loads must preserve other registers, carry and upper flags, update NZ "
            "and clear V");
    reference.set_registers(seed);
    require(result == reference.run(1) && prepared.state() == reference.state() &&
                prepared.progress() == reference.progress(),
            "The independently checked load must also match retained native execution");
    compare_prefixes(prepared, reference, seed, static_cast<Address>(entry + length));
  }
  require(bytes == original &&
              prepared.statistics().compiled_entries == statistics.compiled_entries,
          "Prepared load execution must neither alter source nor trigger lazy translation");
}
}

int main() try {
  constexpr std::array operations{
      Load{.opcode = isa::Opcode::ldaa_immediate, .target = Target::a, .word = false},
      Load{.opcode = isa::Opcode::ldab_immediate, .target = Target::b, .word = false},
      Load{.opcode = isa::Opcode::ldd_immediate, .target = Target::d, .word = true},
      Load{.opcode = isa::Opcode::ldx_immediate, .target = Target::x, .word = true},
      Load{.opcode = isa::Opcode::ldy_immediate, .target = Target::y, .word = true},
      Load{.opcode = isa::Opcode::lds_immediate, .target = Target::sp, .word = true}};
  constexpr std::array<Address, 8> values{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff};
  for (const auto operation : operations) {
    for (const auto value : values) { check(operation, value); }
  }
  std::cout << "Prepared immediate loads preserve full state, CCR, prefixes and cycle deadlines\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
