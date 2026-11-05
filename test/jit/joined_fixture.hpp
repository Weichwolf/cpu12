// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/isa.hpp"
#include "cpu12/memory.hpp"
#include "jit/arena.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace cpu12::test::joined {
using Encoded = std::vector<Byte>;
enum class Edge : std::uint8_t { complete = 0x55, fault = 0xaa };
inline constexpr Registers seed{
    .pc = 0xfffe, .d = 0xa55a, .x = 0x1234, .y = 0xabcd, .sp = 0x8000, .ccr = 0xc0};

void require(bool condition, const char *message);
[[nodiscard]] detail::NativeContext fresh_context();

template <isa::Opcode operation, std::size_t size>
Encoded instruction(const std::array<Byte, size> operands) {
  constexpr auto prefix = isa::opcode<operation>();
  Encoded result(prefix.begin(), prefix.end());
  result.insert(result.end(), operands.begin(), operands.end());
  return result;
}

// A test harness for private emitter fragments, not Core::run or its scheduler.
// Complete edges link to the next authored fragment; faults always exit to host.
class Translation {
public:
  Translation(std::initializer_list<Encoded> instructions, const Memory &memory);
  [[nodiscard]] Edge run(detail::NativeContext &context) const noexcept;

private:
  detail::ExecutableArena arena_;
  const void *entry_ = nullptr;
};

void branches();
void loops();
void faults();
void stop();
} // namespace cpu12::test::joined
