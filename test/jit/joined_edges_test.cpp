// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "joined_fixture.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>

namespace cpu12::test::joined {
namespace {
constexpr unsigned flag_c = 1, flag_v = 2, flag_z = 4, flag_n = 8, byte_mask = 0xff;
constexpr std::array branch_operations{isa::Opcode::bra_relative,
                                       isa::Opcode::brn_relative,
                                       isa::Opcode::bhi_relative,
                                       isa::Opcode::bls_relative,
                                       isa::Opcode::bcc_relative,
                                       isa::Opcode::bcs_relative,
                                       isa::Opcode::bne_relative,
                                       isa::Opcode::beq_relative,
                                       isa::Opcode::bvc_relative,
                                       isa::Opcode::bvs_relative,
                                       isa::Opcode::bpl_relative,
                                       isa::Opcode::bmi_relative,
                                       isa::Opcode::bge_relative,
                                       isa::Opcode::blt_relative,
                                       isa::Opcode::bgt_relative,
                                       isa::Opcode::ble_relative};

std::array<bool, 16> decisions(const unsigned ccr) {
  const bool carry = (ccr & flag_c) != 0;
  const bool zero = (ccr & flag_z) != 0;
  const bool negative = (ccr & flag_n) != 0;
  const bool overflow = (ccr & flag_v) != 0;
  return {true,
          false,
          !carry && !zero,
          carry || zero,
          !carry,
          carry,
          !zero,
          zero,
          !overflow,
          overflow,
          !negative,
          negative,
          negative == overflow,
          negative != overflow,
          !zero && negative == overflow,
          zero || negative != overflow};
}

void linear_flags() {
  std::array<Byte, address_bytes> ram{};
  const Translation translation(
      {instruction<isa::Opcode::ldaa_immediate>(std::array<Byte, 1>{0x7f}),
       instruction<isa::Opcode::adda_immediate>(std::array<Byte, 1>{1}),
       instruction<isa::Opcode::bne_relative>(std::array<Byte, 1>{4})},
      Memory::unified(ram));
  auto context = fresh_context();
  auto expected = seed;
  expected.pc = 8; // FFFE + 2 + 2 + 2 + taken displacement 4, wrapping at 16 bits.
  constexpr Address result_d = 0x805a;
  constexpr Byte result_ccr = 0xea; // Unchanged S/X plus H/N/V; C/Z clear.
  expected.d = result_d;
  expected.ccr = result_ccr;
  require(translation.run(context) == Edge::complete && context.cpu.registers == expected &&
              context.fault == Fault::none && ram == std::array<Byte, address_bytes>{},
          "Joined LDAA/ADDA/BNE preserves cross-instruction flags and complete RAM");
}
} // namespace

void branches() {
  std::array<Byte, address_bytes> ram{};
  constexpr std::array<int, 3> displacements{-128, 0, 127};
  for (const auto displacement : displacements) {
    for (std::size_t condition = 0; condition < branch_operations.size(); ++condition) {
      const Translation translation({Encoded{static_cast<Byte>(branch_operations[condition]),
                                             static_cast<Byte>(displacement)}},
                                    Memory::unified(ram));
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        auto context = fresh_context();
        context.cpu.registers.ccr = static_cast<Byte>(ccr);
        auto expected = context.cpu.registers;
        expected.pc =
            static_cast<Address>(int{seed.pc} + 2 + (decisions(ccr)[condition] ? displacement : 0));
        require(translation.run(context) == Edge::complete && context.cpu.registers == expected &&
                    context.fault == Fault::none,
                "All joined branch arms match independent CCR predicates and wrapping PC");
      }
    }
  }
  linear_flags();
}

void loops() {
  std::array<Byte, address_bytes> ram{};
  constexpr Byte x_counter_nonzero = 0x25;
  constexpr Byte negative_displacement = 0x10;
  constexpr std::array<int, 3> displacements{-256, 0, 255};
  constexpr std::array<Address, 5> values{0, 1, 2, 0x8000, 0xffff};
  for (const auto displacement : displacements) {
    const Translation translation(
        {instruction<isa::Opcode::dbne_relative>(std::array<Byte, 2>{
            static_cast<Byte>(x_counter_nonzero | (displacement < 0 ? negative_displacement : 0)),
            static_cast<Byte>(displacement)})},
        Memory::unified(ram));
    for (const auto value : values) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        auto context = fresh_context();
        context.cpu.registers.x = value;
        context.cpu.registers.ccr = static_cast<Byte>(ccr);
        auto expected = context.cpu.registers;
        constexpr unsigned word_mask = 0xffff;
        expected.x = static_cast<Address>((unsigned{value} + word_mask) & word_mask);
        expected.pc = static_cast<Address>(int{seed.pc} + 3 + (expected.x != 0 ? displacement : 0));
        require(translation.run(context) == Edge::complete && context.cpu.registers == expected &&
                    context.fault == Fault::none,
                "Joined DBNE uses the decremented word, preserves every CCR and wraps its target");
      }
    }
  }
}
} // namespace cpu12::test::joined

int main() {
  try {
    cpu12::test::joined::branches();
    cpu12::test::joined::loops();
    cpu12::test::joined::faults();
    cpu12::test::joined::stop();
    std::cout
        << "Joined emitter edges: independent branches, loops, flags, bus faults and STOP passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
