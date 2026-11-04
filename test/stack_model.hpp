// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cpu12::test::stack {
using Byte = std::uint8_t;
constexpr unsigned byte_bits = 8;
constexpr unsigned byte_mask = 0xff;
constexpr unsigned high_mask = 0xff00;
constexpr unsigned x_mask = 0x40;
constexpr std::size_t memory_bytes = 65536;
constexpr std::array<Registers, 3> seeds{
    {{.pc = 0xfffe, .d = 0x80ff, .x = 0x1234, .y = 0xabcd, .sp = 1, .ccr = 0xff},
     {.pc = 0, .d = 0x7f80, .x = 0xffff, .y = 0, .sp = 0xffff, .ccr = 0x21},
     {.pc = 0x7fff, .d = 0, .x = 0x8000, .y = 0xffff, .sp = 0, .ccr = 0x12}}};
inline std::uint64_t checked = 0;

enum class Target : std::uint8_t { x, y, a, b, ccr, d, pc };

struct StackOp {
  Byte opcode;
  Target target;
  unsigned width;
  bool push;
};

constexpr std::array<StackOp, 13> operations{
    {{.opcode = 0x30, .target = Target::x, .width = 2, .push = false},
     {.opcode = 0x31, .target = Target::y, .width = 2, .push = false},
     {.opcode = 0x32, .target = Target::a, .width = 1, .push = false},
     {.opcode = 0x33, .target = Target::b, .width = 1, .push = false},
     {.opcode = 0x34, .target = Target::x, .width = 2, .push = true},
     {.opcode = 0x35, .target = Target::y, .width = 2, .push = true},
     {.opcode = 0x36, .target = Target::a, .width = 1, .push = true},
     {.opcode = 0x37, .target = Target::b, .width = 1, .push = true},
     {.opcode = 0x38, .target = Target::ccr, .width = 1, .push = false},
     {.opcode = 0x39, .target = Target::ccr, .width = 1, .push = true},
     {.opcode = 0x3a, .target = Target::d, .width = 2, .push = false},
     {.opcode = 0x3b, .target = Target::d, .width = 2, .push = true},
     {.opcode = 0x3d, .target = Target::pc, .width = 2, .push = false}}};

inline void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

inline std::uint16_t value(const Registers before, const Target target) {
  switch (target) {
    case Target::x: return before.x;
    case Target::y: return before.y;
    case Target::a: return static_cast<std::uint16_t>(before.d >> byte_bits);
    case Target::b: return static_cast<std::uint16_t>(before.d & byte_mask);
    case Target::ccr: return before.ccr;
    case Target::d: return before.d;
    case Target::pc: return before.pc;
    default: throw std::runtime_error("Unknown independently authored stack target");
  }
}

inline void assign(Registers &state, const Target target, const std::uint16_t operand) {
  switch (target) {
    case Target::x: state.x = operand; break;
    case Target::y: state.y = operand; break;
    case Target::a:
      state.d =
          static_cast<std::uint16_t>((state.d & byte_mask) | ((operand & byte_mask) << byte_bits));
      break;
    case Target::b:
      state.d = static_cast<std::uint16_t>((state.d & high_mask) | (operand & byte_mask));
      break;
    case Target::ccr:
      state.ccr =
          static_cast<Byte>(operand & (byte_mask ^ ((state.ccr & x_mask) == 0 ? x_mask : 0U)));
      break;
    case Target::d: state.d = operand; break;
    case Target::pc: state.pc = operand; break;
    default: throw std::runtime_error("Unknown independently authored stack destination");
  }
}

inline std::uint16_t next(const std::uint16_t address) {
  return static_cast<std::uint16_t>(static_cast<unsigned>(address) + 1U);
}

inline void word(std::span<Byte> ram, const std::uint16_t address, const std::uint16_t operand) {
  ram[address] = static_cast<Byte>(operand >> byte_bits);
  ram[next(address)] = static_cast<Byte>(operand);
}

inline std::uint16_t word(const std::span<const Byte> ram, const std::uint16_t address) {
  return static_cast<std::uint16_t>((static_cast<unsigned>(ram[address]) << byte_bits) |
                                    ram[next(address)]);
}

inline Registers expected(const StackOp operation, Registers before, const std::uint16_t operand) {
  before.sp = static_cast<std::uint16_t>(
      static_cast<int>(before.sp) +
      (operation.push ? -static_cast<int>(operation.width) : static_cast<int>(operation.width)));
  if (operation.target != Target::pc) { before.pc = next(before.pc); }
  if (!operation.push) { assign(before, operation.target, operand); }
  return before;
}

inline void retired(Core &core,
                    const std::span<const Byte> code,
                    const Registers before,
                    const Registers wanted) {
  core.set_registers(before);
  const auto result = core.run_instruction(code);
  require(result.retired == 1 && result.fault == Fault::none && result.pc == before.pc &&
              result.opcode == code[0] && core.registers() == wanted,
          "Independent stack/call full-register expectation failed");
  ++checked;
}

inline void call(Core &core,
                 const std::span<Byte> ram,
                 const std::span<const Byte> code,
                 const Registers before,
                 const Registers wanted) {
  retired(core, code, before, wanted);
  require(word(ram, wanted.sp) == static_cast<std::uint16_t>(before.pc + code.size()),
          "A call really stores its complete following address, high byte first, including wrap");
}

void calls();
void external_bus();
}
