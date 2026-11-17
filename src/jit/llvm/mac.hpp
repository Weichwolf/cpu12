// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "instruction.hpp"
#include "jit/llvm/flags.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/memory.hpp"

#include "llvm-c/Core.h"

namespace cpu12::detail::llvm19 {
template <class Transfer>
void lower_mac(Builder &b, Values &state, const Address accumulator, Transfer transfer) {
  constexpr unsigned word_bits = 16;
  constexpr unsigned word_mask = 0xffff;
  constexpr unsigned carry = 1;
  constexpr unsigned overflow = 2;
  const Instruction read{.kind = Kind::load, .addressing = Addressing::indexed, .word = true};
  const Instruction write{.kind = Kind::store, .addressing = Addressing::indexed, .word = true};
  const auto load = [&](LLVMValueRef at) { return transfer(read, AccessValues{.address = at}); };
  // Preserve bus order and read all old operands before either result word is written.
  auto *left = load(state.x);
  auto *right = load(state.y);
  auto *high = load(b.getInt32(accumulator));
  auto *low = load(b.getInt32(static_cast<Address>(accumulator + 2)));
  left = LLVMBuildSExt(b.get(), b.CreateTrunc(left, b.getInt16Ty()), b.getInt32Ty(), "");
  right = LLVMBuildSExt(b.get(), b.CreateTrunc(right, b.getInt16Ty()), b.getInt32Ty(), "");
  auto *product = LLVMBuildMul(b.get(), left, right, "");
  auto *old = b.CreateOr(b.CreateShl(high, b.getInt32(word_bits)), low);
  auto *result = b.CreateAdd(old, product);
  auto *signs = b.CreateAnd(b.CreateNot(b.CreateXor(old, product)), b.CreateXor(old, result));
  auto *v = b.CreateAnd(b.CreateLShr(signs, b.getInt32((2 * word_bits) - 2)), b.getInt32(overflow));
  auto *sum = b.CreateAdd(low, b.CreateAnd(product, b.getInt32(word_mask)));
  auto *c = b.CreateAnd(b.CreateLShr(sum, b.getInt32(word_bits)), b.getInt32(carry));
  static_cast<void>(transfer(write,
                             AccessValues{.address = b.getInt32(accumulator),
                                          .value = b.CreateLShr(result, b.getInt32(word_bits))}));
  static_cast<void>(transfer(
      write,
      AccessValues{.address = b.getInt32(static_cast<Address>(accumulator + 2)), .value = result}));
  state.ccr = flags(
      b, state.ccr, {.value = result, .carry_bit = c, .overflow_bit = v, .width = 2 * word_bits});
}
}
