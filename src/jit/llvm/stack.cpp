// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/stack.hpp"

#include "instruction.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/lift.hpp"

#include "llvm-c/Types.h"

#include <cstdint>
#include <stdexcept>

namespace cpu12::detail::llvm19 {
namespace {
constexpr unsigned byte_bits = 8, byte_mask = 0xff, high_mask = 0xff00, word_mask = 0xffff;
constexpr unsigned xirq = 0x40;
}

bool stack_operand(const Instruction &instruction) noexcept {
  return instruction.kind == Kind::push || instruction.kind == Kind::pull ||
         instruction.kind == Kind::return_subroutine || instruction.kind == Kind::call;
}

LLVMValueRef stack_register(Builder &b, const Values &s, const StackTarget target) {
  switch (target) {
    case StackTarget::x: return s.x;
    case StackTarget::y: return s.y;
    case StackTarget::a: return b.CreateLShr(s.d, b.getInt32(byte_bits));
    case StackTarget::b: return b.CreateAnd(s.d, b.getInt32(byte_mask));
    case StackTarget::ccr: return s.ccr;
    case StackTarget::d: return s.d;
    case StackTarget::pc:
    default: throw std::logic_error("CPU12 LLVM stack source has no PC register value");
  }
}

void stack_register(Builder &b, Values &s, const StackTarget target, LLVMValueRef value) {
  switch (target) {
    case StackTarget::x: s.x = value; break;
    case StackTarget::y: s.y = value; break;
    case StackTarget::a:
      s.d = b.CreateOr(b.CreateAnd(s.d, b.getInt32(byte_mask)),
                       b.CreateShl(value, b.getInt32(byte_bits)));
      break;
    case StackTarget::b: s.d = b.CreateOr(b.CreateAnd(s.d, b.getInt32(high_mask)), value); break;
    case StackTarget::ccr:
      s.ccr = b.CreateAnd(value, b.CreateOr(s.ccr, b.getInt32(byte_mask ^ xirq)));
      break;
    case StackTarget::d: s.d = value; break;
    case StackTarget::pc:
    default: throw std::logic_error("CPU12 LLVM stack PC requires a control-flow destination");
  }
}

LLVMValueRef stack_address(Builder &b, const Values &s, const int delta) {
  return b.CreateAnd(b.CreateAdd(s.sp, b.getInt32(static_cast<std::uint16_t>(delta))),
                     b.getInt32(word_mask));
}
}
