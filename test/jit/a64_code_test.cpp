// SPDX-License-Identifier: GPL-3.0-only
#include "jit/a64/code.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
#if defined(__aarch64__)
#include "jit/arena.hpp"

#include <bit>
#endif

namespace {
using namespace cpu12::detail::a64;

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

std::uint32_t instruction(const Code &code, const std::size_t index) {
  constexpr unsigned byte_bits = 8;
  std::uint32_t result = 0;
  for (unsigned byte = 0; byte < sizeof(result); ++byte) {
    result |= std::uint32_t{code.bytes().at((index * sizeof(result)) + byte)} << (byte * byte_bits);
  }
  return result;
}

void encodings() {
  Code code;
  code.alu(Alu::add, Reg::x0, Reg::x1, Reg::x2);
  code.alu(Alu::subtract_flags, Reg::x0, Reg::x1, Reg::x2);
  code.alu(Alu::bit_clear, Reg::x0, Reg::x1, Reg::x2);
  code.shift(Shift::left, Reg::x0, Reg::x1, 8);
  code.shift(Shift::right, Reg::x0, Reg::x1, 8);
  code.shift(Shift::arithmetic, Reg::x0, Reg::x1, 8);
  code.shift(Shift::rotate, Reg::x0, Reg::x1, 8);
  code.reverse_half(Reg::x0, Reg::x1);
  code.extract(Reg::x0, Reg::x1, 0, 8, Width::w32, true);
  code.load(Reg::x0, Reg::x1, 5, Access::byte);
  code.load(Reg::x0, Reg::x1, 6, Access::half);
  code.load(Reg::x0, Reg::x1, 8, Access::doubleword);
  code.push(Reg::x19, Reg::x20);
  code.pop(Reg::x19, Reg::x20);
  code.set(Reg::x0, Condition::equal);
  code.select(Reg::x0, Reg::x1, Reg::x2, Condition::equal);
  code.read_flags(Reg::x0);
  code.write_flags(Reg::x0);
  code.multiply(Reg::x0, Reg::x1, Reg::x2);
  code.divide(Reg::x0, Reg::x1, Reg::x2, true);
  code.divide(Reg::x0, Reg::x1, Reg::x2, false);
  code.multiply_subtract(Reg::x0, Reg::x1, Reg::x2, Reg::x3);
  code.call(Reg::x16);
  code.return_();
  code.insert(Reg::x21, Reg::x2, 8, 8);
  code.insert(Reg::x0, Reg::x1, 0, 16);
  code.subtract(Reg::x0, Reg::x1, 1);
  constexpr std::uint16_t maximum_immediate = 4095;
  code.subtract(Reg::x2, Reg::x3, maximum_immediate, Width::x64);
  code.scalar_move(Reg::x2, 16, true);
  code.scalar_move(Reg::x2, 16, false);
  constexpr std::uint16_t radius = 1024;
  code.compare(Reg::x21, radius);
  code.compare(Reg::x3, maximum_immediate, Width::x64);
  code.compare_if(Reg::x2, Reg::x15, Condition::no_carry);
  code.compare_if(Reg::x9, Reg::x10, Condition::carry, Width::x64);
  code.insert(Reg::x0, Reg::x22, 16, 16, Width::x64);
  code.insert(Reg::x0, Reg::x23, 32, 16, Width::x64);
  constexpr unsigned return_position = 48;
  code.insert(Reg::x0, Reg::x1, return_position, 16, Width::x64);
  code.reverse_half(Reg::x0, Reg::x0, Width::x64);
  constexpr std::array<std::uint32_t, 38> expected{
      0x0b020020, 0x6b020020, 0x0a220020, 0x53185c20, 0x53087c20, 0x13087c20, 0x13812020,
      0x5ac00420, 0x13001c20, 0x39401420, 0x79400c20, 0xf9400420, 0xa9bf53f3, 0xa8c153f3,
      0x1a9f17e0, 0x1a820020, 0xd53b4200, 0xd51b4200, 0x1b027c20, 0x1ac20c20, 0x1ac20820,
      0x1b028c20, 0xd63f0200, 0xd65f03c0, 0x33181c55, 0x33003c20, 0x51000420, 0xd13ffc62,
      0x1e270050, 0x1e260202, 0x711002bf, 0xf13ffc7f, 0x7a4f3040, 0xfa4a2120, 0xb3703ec0,
      0xb3603ee0, 0xb3503c20, 0xdac00400};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    require(instruction(code, index) == expected[index], "A64 encoding differs from ISA reference");
  }
}

void fixups() {
  Code code;
  Label begin;
  Label end;
  code.bind(begin);
  code.branch(end);
  code.branch(Condition::not_equal, begin);
  code.branch_zero(Reg::x2, end);
  code.branch_zero(Reg::x3, begin, Width::x64, true);
  code.bind(end);
  constexpr std::array<std::uint32_t, 4> expected{0x14000004, 0x54ffffe1, 0x34000042, 0xb5ffffa3};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    require(instruction(code, index) == expected[index],
            "A64 forward/backward branch fixup failed");
  }
  bool rejected = false;
  try {
    code.bind(end);
  } catch (const std::logic_error &) { rejected = true; }
  require(rejected, "A64 labels must not be rebound");
}

void far_fixups() {
  Code code;
  Label begin;
  Label end;
  code.bind(begin);
  code.branch_far(Condition::equal, end);
  code.branch_zero_far(Reg::x0, end, Width::x64, true);
  code.branch_far(Condition::not_equal, begin);
  code.bind(end);
  constexpr std::array<std::uint32_t, 6> expected{
      0x540000c0, 0xd503201f, 0xb5000080, 0xd503201f, 0x54ffff81, 0xd503201f};
  for (std::size_t index = 0; index < expected.size(); ++index) {
    require(instruction(code, index) == expected[index],
            "Near A64 conditional branch relaxation failed");
  }
  Code distant;
  Label beyond;
  distant.branch_far(Condition::equal, beyond);
  constexpr std::size_t padding_bytes = 1U << 20;
  const std::vector<std::uint8_t> padding(padding_bytes);
  distant.append(padding);
  distant.bind(beyond);
  constexpr std::uint32_t inverse = 0x54000041;
  constexpr std::uint32_t jump = 0x14040001;
  require(instruction(distant, 0) == inverse && instruction(distant, 1) == jump,
          "Distant A64 branches must retain their complete long form");
}

#if defined(__aarch64__)
extern "C" std::uint64_t cpu12_a64_test_enter(const void *, std::uint64_t, std::uint64_t) noexcept;
#if defined(__APPLE__)
asm(".text\n"
    ".globl _cpu12_a64_test_enter\n"
    "_cpu12_a64_test_enter:\n"
    "mov x16,x0\n"
    "mov x0,x1\n"
    "mov x1,x2\n"
    "br x16\n");
#else
asm(".text\n"
    ".globl cpu12_a64_test_enter\n"
    ".type cpu12_a64_test_enter,%function\n"
    "cpu12_a64_test_enter:\n"
    "mov x16,x0\n"
    "mov x0,x1\n"
    "mov x1,x2\n"
    "br x16\n");
#endif

void conditional_comparisons() {
  cpu12::detail::ExecutableArena arena;
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  constexpr std::array<std::uint64_t, 3> deadlines{0, 5, maximum};
  constexpr std::array<std::uint64_t, 4> starts{0, 1, maximum - 5, maximum};
  constexpr std::array<std::uint64_t, 3> charges{0, 1, 10};
  constexpr unsigned required = 2;
  for (const unsigned budget : {0U, 1U, 2U, 3U}) {
    for (const auto deadline : deadlines) {
      Code code;
      code.constant(Reg::x3, budget);
      code.constant(Reg::x4, required);
      code.constant(Reg::x5, deadline);
      code.alu(Alu::add_flags, Reg::x2, Reg::x0, Reg::x1, Width::x64);
      code.compare_if(Reg::x3, Reg::x4, Condition::no_carry);
      code.compare_if(Reg::x5, Reg::x2, Condition::carry, Width::x64);
      code.set(Reg::x0, Condition::carry);
      code.return_();
      const auto *entry = arena.append(code.bytes());
      for (const auto start : starts) {
        for (const auto charge : charges) {
          const bool admitted =
              budget >= required && charge <= deadline && start <= deadline - charge;
          require(cpu12_a64_test_enter(entry, start, charge) == std::uint64_t{admitted},
                  "Conditional A64 admission must reject budget, deadline and addition overflow "
                  "independently");
        }
      }
    }
  }
}

void execute() {
  cpu12::detail::ExecutableArena arena;
  Code code;
  Label again;
  code.constant(Reg::x2, 0);
  code.bind(again);
  code.alu(Alu::add, Reg::x2, Reg::x2, Reg::x1, Width::x64);
  code.constant(Reg::x3, 1);
  code.alu(Alu::subtract_flags, Reg::x0, Reg::x0, Reg::x3, Width::x64);
  code.branch(Condition::not_equal, again);
  code.move(Reg::x0, Reg::x2);
  code.return_();
  const auto *entry = arena.append(code.bytes());
  constexpr std::uint64_t count = 31;
  constexpr std::uint64_t increment = 0x123456789abcdef;
  require(cpu12_a64_test_enter(entry, count, increment) == count * increment,
          "A64 native loop must preserve 64-bit operands and backward branches");
  Code negative;
  constexpr auto signed_value = std::int64_t{-123456789};
  constexpr unsigned shift = 17;
  negative.constant(Reg::x0, std::bit_cast<std::uint64_t>(signed_value));
  negative.shift(Shift::arithmetic, Reg::x0, Reg::x0, shift, Width::x64);
  negative.return_();
  require(cpu12_a64_test_enter(arena.append(negative.bytes()), 0, 0) ==
              std::bit_cast<std::uint64_t>(signed_value >> shift),
          "A64 64-bit immediate construction and arithmetic shifts failed");
  require(cpu12_a64_test_enter(entry, count, increment) == count * increment,
          "Publishing a second native function changed a retained translation");
}
#endif
}

int main() {
  try {
    encodings();
    fixups();
    far_fixups();
#if defined(__aarch64__)
    execute();
    conditional_comparisons();
#endif
    std::cout << "A64 encoding and branch checks passed.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
