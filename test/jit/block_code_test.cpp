// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace cpu12;
constexpr Address writer_pc = 0x8000, target_pc = 0x8100, alias_pc = 0x9000;
constexpr Byte original = 0x11, replacement = 0x22;
constexpr unsigned byte_bits = 8;
constexpr unsigned byte_mask = 0xff;
constexpr Registers seed{
    .pc = target_pc, .d = 0x135a, .x = 0x1234, .y = 0xabcd, .sp = 0xa000, .ccr = 0xc0};
constexpr auto target =
    isa::program(isa::opcode<isa::Opcode::ldaa_immediate>(), std::array<Byte, 1>{original});

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

template <Address destination> consteval auto writer() {
  constexpr auto address = destination + 1;
  constexpr std::array<Byte, 2> store_address{static_cast<Byte>(address >> byte_bits),
                                              static_cast<Byte>(address)};
  constexpr std::array<Byte, 2> jump_address{static_cast<Byte>(target_pc >> byte_bits),
                                             static_cast<Byte>(target_pc)};
  return isa::program(isa::opcode<isa::Opcode::ldaa_immediate>(),
                      std::array<Byte, 1>{replacement},
                      isa::opcode<isa::Opcode::staa_extended>(),
                      store_address,
                      isa::opcode<isa::Opcode::jmp_extended>(),
                      jump_address);
}

Registers loaded(const Byte value) {
  auto expected = seed;
  expected.pc = target_pc + target.size();
  expected.d = static_cast<Address>((unsigned{value} << byte_bits) | (seed.d & byte_mask));
  return expected; // Both literal operands are positive/nonzero; LDAA clears N/Z/V.
}

void warm(Core &core) {
  core.set_registers(seed);
  const auto result = core.run(1);
  require(result.retired == 1 && result.fault == Fault::none &&
              core.registers() == loaded(original),
          "Initial Core::run executes and warms the original target");
}

void check_replacement(Core &core, const unsigned budget, const char *scope, const Byte &operand) {
  const auto result = core.run(budget);
  if (result.retired != budget || result.fault != Fault::none ||
      core.registers() != loaded(replacement)) {
    throw std::runtime_error(std::string(scope) +
                             " expected_D=" + std::to_string(loaded(replacement).d) +
                             " actual_D=" + std::to_string(core.registers().d) +
                             " retired=" + std::to_string(result.retired) +
                             " actual_operand=" + std::to_string(operand));
  }
}

void host_change() {
  std::array<Byte, address_bytes> ram{};
  std::ranges::copy(target, ram.begin() + target_pc);
  Core core(Memory::unified(ram));
  warm(core);
  ram[target_pc + 1] = replacement;
  const auto expected_memory = ram;
  core.set_registers(seed);
  check_replacement(core, 1, "Host-changed immediate in warmed block", ram[target_pc + 1]);
  require(ram == expected_memory, "Execution preserves complete RAM after the host edit");
}

void guest_change() {
  std::array<Byte, address_bytes> ram{};
  constexpr auto program = writer<target_pc>();
  std::ranges::copy(program, ram.begin() + writer_pc);
  std::ranges::copy(target, ram.begin() + target_pc);
  auto expected_memory = ram;
  expected_memory[target_pc + 1] = replacement;
  Core core(Memory::unified(ram));
  warm(core);
  auto start = seed;
  start.pc = writer_pc;
  core.set_registers(start);
  check_replacement(
      core, 4, "Guest STAA/JMP changes a different warmed target block", ram[target_pc + 1]);
  require(ram == expected_memory, "Guest STAA changes exactly one program byte");
}

void physical_alias(const bool readonly_code) {
  auto program = writer<alias_pc>();
  auto executable = target;
  const std::array<Ram, 3> regions{
      {{.first = writer_pc, .bytes = program},
       {.first = target_pc, .bytes = executable, .writable = !readonly_code},
       {.first = alias_pc, .bytes = executable}}};
  Core core(Memory::mapped(regions));
  warm(core);
  auto start = seed;
  start.pc = writer_pc;
  core.set_registers(start);
  check_replacement(
      core, 4, "Guest STAA through physical alias changes warmed code", executable[1]);
  auto expected = target;
  expected[1] = replacement;
  require(executable == expected && program == writer<alias_pc>(),
          "Aliased store changes the actual instruction operand and no writer bytes");
}

void writable_alias() {
  physical_alias(false);
}

void readonly_alias() {
  physical_alias(true);
}
} // namespace

int main() {
  unsigned failed = 0;
  for (const auto test : {host_change, guest_change, writable_alias, readonly_alias}) {
    try {
      test();
    } catch (const std::exception &error) {
      std::cerr << error.what() << '\n';
      ++failed;
    }
  }
  if (failed != 0) {
    std::cerr << "Failed code-coherence cases=" << failed << '\n';
    return 1;
  }
  std::cout << "Warm Core::run fetches current code after host/guest/physical-alias writes\n";
  return 0;
}
