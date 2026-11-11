// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/flags.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/registers.hpp"
#include "jit/llvm/stack.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Types.h"

#include <array>

namespace cpu12::detail::llvm19 {
namespace {
constexpr unsigned byte_mask = 0xff, word_mask = 0xffff, byte_bits = 8;
constexpr unsigned exchange_mask = 0x80, selector_mask = 7, ccr_selector = 2;
constexpr std::array targets{StackTarget::a,
                             StackTarget::b,
                             StackTarget::ccr,
                             StackTarget::pc,
                             StackTarget::d,
                             StackTarget::x,
                             StackTarget::y};

LLVMValueRef read(Builder &b, const Values &s, const unsigned selector) {
  return selector == selector_mask ? s.sp : stack_register(b, s, targets.at(selector));
}

void write(Builder &b, Values &s, const unsigned selector, LLVMValueRef value) {
  value = b.CreateAnd(value, b.getInt32(selector < 4 ? byte_mask : word_mask));
  if (selector == selector_mask) {
    s.sp = value;
  } else {
    stack_register(b, s, targets.at(selector), value);
  }
}
}

void lower_transfer(Builder &b, Values &s, const Instruction instruction) {
  const auto source = (instruction.operand >> 4U) & selector_mask;
  const auto destination = instruction.operand & selector_mask;
  const bool exchange = (instruction.operand & exchange_mask) != 0;
  auto *first = read(b, s, source);
  if (exchange) {
    auto *second = read(b, s, destination);
    if (source >= 4 && destination < 4 && destination != 0) {
      second = b.CreateOr(second, b.getInt32(word_mask ^ byte_mask));
    }
    write(b, s, source, second);
  } else if (source < 4 && destination >= 4) {
    first = LLVMBuildSExt(b.get(), b.CreateTrunc(first, b.getInt8Ty()), b.getInt32Ty(), "");
  }
  write(b, s, destination, first);
  constexpr unsigned prefix = 0x18;
  constexpr unsigned carry = 1;
  if (instruction.opcode == prefix) {
    s.ccr = flags(b,
                  s.ccr,
                  {.value = first,
                   .carry_bit = b.CreateAnd(s.ccr, b.getInt32(carry)),
                   .overflow_bit = b.getInt32(0),
                   .width = byte_bits});
  }
}

bool transfer_masks(const Instruction instruction) noexcept {
  const auto source = (instruction.operand >> 4U) & selector_mask;
  return instruction.kind == Kind::transfer &&
         ((instruction.operand & selector_mask) == ccr_selector ||
          (source == ccr_selector && (instruction.operand & exchange_mask) != 0));
}
}
