// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/indexed.hpp"

#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/lift.hpp"

#include "llvm-c/Types.h"

#include <cstdint>
#include <stdexcept>

namespace cpu12::detail::llvm19 {
namespace {
constexpr unsigned byte_bits = 8;
constexpr std::uint32_t byte_mask = 0xff, word_mask = 0xffff;

LLVMValueRef &base(Values &state, const IndexBase reg) {
  switch (reg) {
    case IndexBase::x: return state.x;
    case IndexBase::y: return state.y;
    case IndexBase::sp: return state.sp;
    case IndexBase::pc:
    default: break;
  }
  throw std::logic_error("CPU12 PC has no indexed auto update");
}
}

LLVMValueRef effective_address(Builder &builder, Values &state, const BlockInstruction &operation) {
  const auto &instruction = operation.instruction;
  if (instruction.addressing != Addressing::indexed) {
    return builder.getInt32(instruction.address);
  }
  const auto &indexed = instruction.indexed;
  auto *reg = indexed.base == IndexBase::pc
                  ? builder.getInt32(static_cast<Address>(operation.pc + instruction.length))
                  : base(state, indexed.base);
  LLVMValueRef offset = nullptr;
  switch (indexed.offset) {
    case IndexOffset::constant: offset = builder.getInt32(indexed.displacement); break;
    case IndexOffset::a: offset = builder.CreateLShr(state.d, builder.getInt32(byte_bits)); break;
    case IndexOffset::b: offset = builder.CreateAnd(state.d, builder.getInt32(byte_mask)); break;
    case IndexOffset::d: offset = state.d; break;
    default: throw std::logic_error("Invalid CPU12 indexed offset");
  }
  return builder.CreateAnd(builder.CreateAdd(reg, offset), builder.getInt32(word_mask));
}

void commit_index(Builder &builder, Values &state, const Instruction &instruction) {
  if (instruction.addressing != Addressing::indexed || instruction.indexed.update == 0) { return; }
  auto &reg = base(state, instruction.indexed.base);
  const auto delta = static_cast<std::uint16_t>(static_cast<int>(instruction.indexed.update));
  reg = builder.CreateAnd(builder.CreateAdd(reg, builder.getInt32(delta)),
                          builder.getInt32(word_mask));
}
}
