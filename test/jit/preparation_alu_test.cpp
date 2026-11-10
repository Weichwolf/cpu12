// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address first = 0x4000, stride = 8;
constexpr unsigned byte_bits = 8, word_bits = 16, byte_mask = 0xff, word_mask = 0xffff;
constexpr unsigned carry = 1, overflow = 2, zero = 4, negative = 8, half = 0x20;
enum class Operation : Byte { add, adc, sub, sbc, compare, bit_and, bit_or, bit_xor, bit_test };

struct Alu {
  isa::Opcode opcode;
  Operation operation;
  Address Registers::*field = &Registers::d;
  unsigned width = byte_bits, shift = 0;
};

constexpr std::array operations{
    Alu{.opcode = isa::Opcode::adda_immediate, .operation = Operation::add, .shift = byte_bits},
    Alu{.opcode = isa::Opcode::addb_immediate, .operation = Operation::add},
    Alu{.opcode = isa::Opcode::adca_immediate, .operation = Operation::adc, .shift = byte_bits},
    Alu{.opcode = isa::Opcode::adcb_immediate, .operation = Operation::adc},
    Alu{.opcode = isa::Opcode::suba_immediate, .operation = Operation::sub, .shift = byte_bits},
    Alu{.opcode = isa::Opcode::subb_immediate, .operation = Operation::sub},
    Alu{.opcode = isa::Opcode::sbca_immediate, .operation = Operation::sbc, .shift = byte_bits},
    Alu{.opcode = isa::Opcode::sbcb_immediate, .operation = Operation::sbc},
    Alu{.opcode = isa::Opcode::cmpa_immediate, .operation = Operation::compare, .shift = byte_bits},
    Alu{.opcode = isa::Opcode::cmpb_immediate, .operation = Operation::compare},
    Alu{.opcode = isa::Opcode::anda_immediate, .operation = Operation::bit_and, .shift = byte_bits},
    Alu{.opcode = isa::Opcode::andb_immediate, .operation = Operation::bit_and},
    Alu{.opcode = isa::Opcode::oraa_immediate, .operation = Operation::bit_or, .shift = byte_bits},
    Alu{.opcode = isa::Opcode::orab_immediate, .operation = Operation::bit_or},
    Alu{.opcode = isa::Opcode::eora_immediate, .operation = Operation::bit_xor, .shift = byte_bits},
    Alu{.opcode = isa::Opcode::eorb_immediate, .operation = Operation::bit_xor},
    Alu{.opcode = isa::Opcode::bita_immediate,
        .operation = Operation::bit_test,
        .shift = byte_bits},
    Alu{.opcode = isa::Opcode::bitb_immediate, .operation = Operation::bit_test},
    Alu{.opcode = isa::Opcode::addd_immediate, .operation = Operation::add, .width = word_bits},
    Alu{.opcode = isa::Opcode::subd_immediate, .operation = Operation::sub, .width = word_bits},
    Alu{.opcode = isa::Opcode::cpd_immediate, .operation = Operation::compare, .width = word_bits},
    Alu{.opcode = isa::Opcode::cpx_immediate,
        .operation = Operation::compare,
        .field = &Registers::x,
        .width = word_bits},
    Alu{.opcode = isa::Opcode::cpy_immediate,
        .operation = Operation::compare,
        .field = &Registers::y,
        .width = word_bits},
    Alu{.opcode = isa::Opcode::cps_immediate,
        .operation = Operation::compare,
        .field = &Registers::sp,
        .width = word_bits}};
constexpr std::array<Address, 12> word_operands{
    0, 1, 14, 15, 16, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xfffe, 0xffff};

constexpr std::array<Address, 12> byte_operands{
    0, 1, 14, 15, 16, 0x7e, 0x7f, 0x80, 0x81, 0xf0, 0xfe, 0xff};
constexpr Registers initial_registers{.d = 0xa55a, .x = 0x1234, .y = 0x5678, .sp = 0x9000};

const auto &operands(const Alu operation) {
  return operation.width == byte_bits ? byte_operands : word_operands;
}

void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

unsigned mask(const Alu operation) {
  return operation.width == byte_bits ? byte_mask : word_mask;
}

unsigned length(const Alu operation) {
  return operation.width == byte_bits ? 2U : 3U;
}

Registers with_value(Registers state, const Alu operation, const unsigned value) {
  const auto field_mask = mask(operation) << operation.shift;
  state.*operation.field =
      static_cast<Address>(((state.*operation.field) & ~field_mask) | (value << operation.shift));
  return state;
}

int signed_value(const unsigned value, const unsigned modulus) {
  return static_cast<int>(value) - (value < modulus / 2 ? 0 : static_cast<int>(modulus));
}

struct Numeric {
  unsigned value, flags = 0, changed = negative | zero | overflow;
};

Numeric arithmetic_model(const Registers state, const Alu operation, const unsigned right) {
  const auto modulus = mask(operation) + 1;
  const auto left = ((state.*operation.field) >> operation.shift) % modulus;
  const bool add = operation.operation == Operation::add || operation.operation == Operation::adc;
  const auto cin = operation.operation == Operation::adc || operation.operation == Operation::sbc
                       ? state.ccr & carry
                       : 0U;
  const int raw = static_cast<int>(left) +
                  (add ? static_cast<int>(right + cin) : -static_cast<int>(right + cin));
  Numeric result{.value = static_cast<unsigned>(raw + static_cast<int>(modulus)) % modulus};
  const int mathematical =
      signed_value(left, modulus) + (add ? signed_value(right, modulus) + static_cast<int>(cin)
                                         : -signed_value(right, modulus) - static_cast<int>(cin));
  const auto bound = static_cast<int>(modulus / 2);
  result.flags |= mathematical < -bound || mathematical >= bound ? overflow : 0U;
  const bool carry_out = add ? raw >= static_cast<int>(modulus) : raw < 0;
  result.flags |= carry_out ? carry : 0U;
  result.changed |= carry;
  if (add && operation.width == byte_bits) {
    result.changed |= half;
    result.flags |= (left % 16) + (right % 16) + cin >= 16 ? half : 0U;
  }
  return result;
}

Numeric evaluate(const Registers state, const Alu operation, const unsigned right) {
  const auto left = ((state.*operation.field) >> operation.shift) & mask(operation);
  switch (operation.operation) {
    case Operation::bit_and:
    case Operation::bit_test: return {.value = left & right};
    case Operation::bit_or: return {.value = left | right};
    case Operation::bit_xor: return {.value = left ^ right};
    case Operation::add:
    case Operation::adc:
    case Operation::sub:
    case Operation::sbc:
    case Operation::compare: return arithmetic_model(state, operation, right);
    default: throw std::logic_error("Invalid independent immediate ALU operation");
  }
}

Registers expected(Registers state, const Alu operation, const unsigned right) {
  auto result = evaluate(state, operation, right);
  result.flags |= result.value == 0 ? zero : 0U;
  result.flags |= result.value > mask(operation) / 2 ? negative : 0U;
  state.ccr = static_cast<Byte>((state.ccr & (byte_mask ^ result.changed)) | result.flags);
  if (operation.operation != Operation::compare && operation.operation != Operation::bit_test) {
    state = with_value(state, operation, result.value);
  }
  state.pc = static_cast<Address>(state.pc + length(operation));
  return state;
}

struct Fixture {
  Fixture() : bytes(operations.size() * word_operands.size() * stride) {
    for (const auto operation : operations) {
      for (const auto operand : operands(operation)) {
        const auto offset = entries.size() * stride;
        entries.push_back(static_cast<Address>(first + offset));
        bytes[offset] = static_cast<Byte>(operation.opcode);
        bytes[offset + 1] =
            static_cast<Byte>(operation.width == word_bits ? operand >> byte_bits : operand);
        if (operation.width == word_bits) { bytes[offset + 2] = static_cast<Byte>(operand); }
        const auto size = length(operation);
        bytes[offset + size] = static_cast<Byte>(isa::Opcode::bra_relative);
        bytes[offset + size + 1] = static_cast<Byte>(byte_mask - size - 1);
      }
    }
  }

  std::vector<Byte> bytes;
  std::vector<Address> entries;
};

void prefixes(Core &prepared, Core &reference, const Registers seed, const Alu operation) {
  constexpr std::array<std::uint32_t, 8> grants{0, 1, 2, 3, 4, 5, 16, 17};
  constexpr std::array<std::uint64_t, 8> deadlines{0, 1, 2, 3, 4, 5, 16, 17};
  for (const auto pc : {seed.pc, static_cast<Address>(seed.pc + length(operation))}) {
    auto state = seed;
    state.pc = pc;
    for (const auto grant : grants) {
      prepared.set_registers(state);
      reference.set_registers(state);
      require(prepared.run(grant) == reference.run(grant) &&
                  prepared.state() == reference.state() &&
                  prepared.progress() == reference.progress(),
              "Prepared immediate ALU prefixes must retain full state, result and progress");
    }
    for (const auto delta : deadlines) {
      prepared.set_registers(state);
      reference.set_registers(state);
      const auto deadline = prepared.progress().estimated_cycles + delta;
      require(prepared.run_until(deadline) == reference.run_until(deadline) &&
                  prepared.state() == reference.state() &&
                  prepared.progress() == reference.progress(),
              "Prepared immediate ALU must retain atomic cycle deadlines from either entry");
    }
  }
}

void check(Core &prepared,
           Core &reference,
           const Registers seed,
           const Alu operation,
           const unsigned operand) {
  prepared.set_registers(seed);
  const auto before = prepared.progress();
  const auto result = prepared.run(1);
  const RunResult wanted{
      .retired = 1, .pc = seed.pc, .opcode = static_cast<Byte>(operation.opcode)};
  const State state{.registers = expected(seed, operation, operand), .failure = {}};
  const Progress progress{.estimated_cycles =
                              before.estimated_cycles + (operation.width == byte_bits ? 1U : 2U)};
  if (result != wanted || prepared.state() != state || prepared.progress() != progress) {
    throw std::runtime_error("Prepared ALU opcode=" + std::to_string(wanted.opcode) +
                             " D=" + std::to_string(seed.d) + " CCR=" + std::to_string(seed.ccr) +
                             " operand=" + std::to_string(operand));
  }
  reference.set_registers(seed);
  require(reference.run(1) == result && reference.state() == prepared.state() &&
              reference.progress() == prepared.progress(),
          "Independently checked LLVM ALU must also match ordinary native execution");
}

bool prefix_case(const unsigned ccr, const Alu operation, const unsigned value) {
  const bool flags = ccr == 0 || ccr == 1 || ccr == byte_mask - 1 || ccr == byte_mask;
  const bool edge = operation.width == word_bits || value == 0 || value == 1 ||
                    value == byte_mask / 2 || value == byte_mask / 2 + 1 || value == byte_mask;
  return flags && edge;
}

std::size_t check_operand(Core &prepared,
                          Core &reference,
                          const Address entry,
                          const Alu operation,
                          const unsigned operand) {
  const auto count = operation.width == byte_bits ? byte_mask + 1 : word_operands.size();
  for (std::size_t value = 0; value < count; ++value) {
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      auto seed = initial_registers;
      seed.pc = entry;
      seed.ccr = static_cast<Byte>(ccr);
      seed = with_value(seed,
                        operation,
                        operation.width == byte_bits ? static_cast<unsigned>(value)
                                                     : word_operands[value]);
      check(prepared, reference, seed, operation, operand);
      if (prefix_case(ccr, operation, static_cast<unsigned>(value))) {
        prefixes(prepared, reference, seed, operation);
      }
    }
  }
  return count * (byte_mask + 1);
}
}

int main() try {
  Fixture fixture;
  const auto original = fixture.bytes;
  const auto memory = Memory::window({.first = first, .bytes = fixture.bytes, .writable = false});
  Core prepared(memory);
  Core reference(memory);
  const auto initial = prepared.state();
  const auto report = prepared.prepare(fixture.entries);
  require(report.failure.fault == Fault::none && report.unresolved_entries == 0 &&
              report.prepared_entries == fixture.entries.size() * 2 && report.native_bytes > 0 &&
              prepared.state() == initial && prepared.progress() == Progress{},
          "Every immediate ALU opcode and backedge must prepare fully without execution");
  const auto compilations = prepared.statistics().compiled_entries;
  std::size_t index = 0;
  std::size_t cases = 0;
  for (const auto operation : operations) {
    for (const auto operand : operands(operation)) {
      cases += check_operand(prepared, reference, fixture.entries[index++], operation, operand);
    }
  }
  require(fixture.bytes == original && prepared.statistics().compiled_entries == compilations,
          "Prepared ALU runs never change source or lazily translate an instruction");
  std::cout << cases << " independent LLVM immediate ALU state cases and prefix/deadline checks\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
