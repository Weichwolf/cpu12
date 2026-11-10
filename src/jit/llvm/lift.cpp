// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/lift.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/preparation.hpp"
#include "runtime.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>

namespace cpu12::detail::llvm19 {
namespace {
constexpr std::uint32_t carry = 1, overflow = 2, zero = 4, negative = 8;

LLVMValueRef make_function(LLVMModuleRef module, LLVMContextRef context) {
  auto *pointer = LLVMPointerTypeInContext(context, 0);
  std::array fields{LLVMInt64TypeInContext(context), LLVMInt64TypeInContext(context)};
  auto *result = LLVMStructTypeInContext(context, fields.data(), fields.size(), 0);
  std::array parameters{pointer, pointer, LLVMInt32TypeInContext(context)};
  auto *type = LLVMFunctionType(result, parameters.data(), parameters.size(), 0);
  return LLVMAddFunction(module, "cpu12_prepared", type);
}

class FlowLifter {
public:
  FlowLifter(LLVMModuleRef module,
             LLVMContextRef context,
             const std::span<const BlockInstruction> instructions)
      : context_(context),
        function_(make_function(module, context)),
        builder_(context, LLVMAppendBasicBlockInContext(context, function_, "entry")),
        instructions_(instructions),
        registers_(LLVMGetParam(function_, 0)),
        owner_(LLVMGetParam(function_, 1)),
        budget_(LLVMGetParam(function_, 2)),
        done_(LLVMAppendBasicBlockInContext(context, function_, "done")) {}

  LLVMValueRef lift() {
    initialize();
    for (const auto &operation : instructions_) {
      blocks_.emplace(operation.pc,
                      LLVMAppendBasicBlockInContext(
                          context_, function_, ("pc" + std::to_string(operation.pc)).c_str()));
    }
    auto *dispatch = LLVMBuildSwitch(builder_.get(),
                                     load(pc_, builder_.getInt16Ty()),
                                     done_,
                                     static_cast<unsigned>(blocks_.size()));
    for (const auto &[pc, block] : blocks_) { LLVMAddCase(dispatch, builder_.getInt16(pc), block); }
    for (const auto &operation : instructions_) { emit(operation); }
    finish();
    return function_;
  }

private:
  LLVMValueRef address(LLVMValueRef base, std::size_t offset) {
    return builder_.CreateGEP(builder_.getInt8Ty(), base, builder_.getInt64(offset));
  }

  LLVMValueRef load(LLVMValueRef pointer, LLVMTypeRef type) {
    return builder_.CreateLoad(type, pointer);
  }

  void initialize() {
    d_ = LLVMBuildAlloca(builder_.get(), builder_.getInt32Ty(), "d");
    ccr_ = LLVMBuildAlloca(builder_.get(), builder_.getInt32Ty(), "ccr");
    pc_ = LLVMBuildAlloca(builder_.get(), builder_.getInt16Ty(), "pc");
    cycles_ = LLVMBuildAlloca(builder_.get(), LLVMInt64TypeInContext(context_), "cycles");
    retired_ = LLVMBuildAlloca(builder_.get(), builder_.getInt32Ty(), "retired");
    location_ = LLVMBuildAlloca(builder_.get(), LLVMInt64TypeInContext(context_), "location");
    const auto field = [&](std::size_t offset, LLVMTypeRef type) {
      return load(address(registers_, offset), type);
    };
    builder_.CreateStore(
        builder_.CreateZExt(field(offsetof(cpu12::Registers, d), builder_.getInt16Ty()),
                            builder_.getInt32Ty()),
        d_);
    builder_.CreateStore(
        builder_.CreateZExt(field(offsetof(cpu12::Registers, ccr), builder_.getInt8Ty()),
                            builder_.getInt32Ty()),
        ccr_);
    auto *initial_pc = field(offsetof(cpu12::Registers, pc), builder_.getInt16Ty());
    builder_.CreateStore(initial_pc, pc_);
    builder_.CreateStore(builder_.CreateZExt(initial_pc, LLVMInt64TypeInContext(context_)),
                         location_);
    builder_.CreateStore(builder_.getInt32(0), retired_);
    builder_.CreateStore(load(address(owner_, progress_offset), LLVMInt64TypeInContext(context_)),
                         cycles_);
    deadline_ = load(address(owner_, offsetof(cpu12::detail::NativeContext, cycle_deadline)),
                     LLVMInt64TypeInContext(context_));
  }

  void guard(LLVMValueRef completed, LLVMValueRef elapsed) {
    auto *budget_end = LLVMBuildICmp(builder_.get(), LLVMIntUGE, completed, budget_, "");
    auto *deadline_end = LLVMBuildICmp(builder_.get(), LLVMIntUGE, elapsed, deadline_, "");
    auto *body = LLVMAppendBasicBlockInContext(context_, function_, "body");
    LLVMBuildCondBr(builder_.get(), builder_.CreateOr(budget_end, deadline_end), done_, body);
    LLVMPositionBuilderAtEnd(builder_.get(), body);
  }

  LLVMValueRef branch_condition(const cpu12::detail::Instruction &instruction, LLVMValueRef ccr) {
    if (instruction.condition == 0) { return LLVMConstInt(LLVMInt1TypeInContext(context_), 1, 0); }
    if (instruction.condition == 1) { return LLVMConstInt(LLVMInt1TypeInContext(context_), 0, 0); }
    constexpr unsigned first_signed = 12;
    LLVMValueRef bits = nullptr;
    if (instruction.condition >= first_signed) {
      bits = builder_.CreateAnd(
          builder_.CreateXor(ccr, builder_.CreateLShr(ccr, builder_.getInt32(2))),
          builder_.getInt32(overflow));
      if (instruction.condition >= first_signed + 2) {
        bits = builder_.CreateOr(bits, builder_.CreateAnd(ccr, builder_.getInt32(zero)));
      }
    } else {
      constexpr std::array masks{0U,
                                 0U,
                                 carry | zero,
                                 carry | zero,
                                 carry,
                                 carry,
                                 zero,
                                 zero,
                                 overflow,
                                 overflow,
                                 negative,
                                 negative};
      bits = builder_.CreateAnd(ccr, builder_.getInt32(masks.at(instruction.condition)));
    }
    return (instruction.condition & 1U) == 0 ? builder_.CreateICmpEQ(bits, builder_.getInt32(0))
                                             : builder_.CreateICmpNE(bits, builder_.getInt32(0));
  }

  void emit(const cpu12::detail::BlockInstruction &operation) {
    LLVMPositionBuilderAtEnd(builder_.get(), blocks_.at(operation.pc));
    auto *completed = load(retired_, builder_.getInt32Ty());
    auto *elapsed = load(cycles_, LLVMInt64TypeInContext(context_));
    guard(completed, elapsed);
    const auto &instruction = operation.instruction;
    Values state{.d = load(d_, builder_.getInt32Ty()), .ccr = load(ccr_, builder_.getInt32Ty())};
    const auto next = static_cast<cpu12::Address>(operation.pc + instruction.length);
    const auto timing = cpu12::detail::instruction_timing(instruction);
    LLVMValueRef taken = nullptr;
    LLVMValueRef destination = builder_.getInt16(next);
    LLVMValueRef cost = builder_.getInt64(timing.base);
    if (instruction.kind == Kind::branch) {
      taken = branch_condition(instruction, state.ccr);
      destination = builder_.CreateSelect(
          taken,
          builder_.getInt16(static_cast<cpu12::Address>(next + instruction.operand)),
          destination);
      cost =
          builder_.CreateSelect(taken, builder_.getInt64(timing.base + timing.taken_extra), cost);
    } else if (instruction.kind != Kind::nop) {
      lower(builder_, state, instruction);
    }
    builder_.CreateStore(state.d, d_);
    builder_.CreateStore(state.ccr, ccr_);
    builder_.CreateStore(destination, pc_);
    builder_.CreateStore(builder_.CreateAdd(elapsed, cost), cycles_);
    builder_.CreateStore(builder_.CreateAdd(completed, builder_.getInt32(1)), retired_);
    const auto location =
        static_cast<std::uint64_t>(operation.pc) |
        (static_cast<std::uint64_t>(instruction.opcode) << cpu12::detail::native_opcode_shift);
    builder_.CreateStore(builder_.getInt64(location), location_);
    const auto flow = cpu12::detail::successors(operation);
    if (flow.indirect || flow.count == 0) {
      throw std::runtime_error("CPU12 LLVM lowering requires direct flow");
    }
    if (flow.count == 1) {
      LLVMBuildBr(builder_.get(), target(flow.direct[0]));
    } else {
      LLVMBuildCondBr(builder_.get(), taken, target(flow.direct[1]), target(flow.direct[0]));
    }
  }

  [[nodiscard]] LLVMBasicBlockRef target(Address address) const {
    const auto found = blocks_.find(address);
    return found == blocks_.end() ? done_ : found->second;
  }

  void finish() {
    LLVMPositionBuilderAtEnd(builder_.get(), done_);
    builder_.CreateStore(
        builder_.CreateTrunc(load(d_, builder_.getInt32Ty()), builder_.getInt16Ty()),
        address(registers_, offsetof(cpu12::Registers, d)));
    builder_.CreateStore(
        builder_.CreateTrunc(load(ccr_, builder_.getInt32Ty()), builder_.getInt8Ty()),
        address(registers_, offsetof(cpu12::Registers, ccr)));
    builder_.CreateStore(load(pc_, builder_.getInt16Ty()),
                         address(registers_, offsetof(cpu12::Registers, pc)));
    builder_.CreateStore(load(cycles_, LLVMInt64TypeInContext(context_)),
                         address(owner_, progress_offset));
    auto *result = LLVMGetUndef(LLVMGetReturnType(LLVMGlobalGetValueType(function_)));
    result = LLVMBuildInsertValue(builder_.get(),
                                  result,
                                  builder_.CreateZExt(load(retired_, builder_.getInt32Ty()),
                                                      LLVMInt64TypeInContext(context_)),
                                  0,
                                  "");
    result = LLVMBuildInsertValue(
        builder_.get(), result, load(location_, LLVMInt64TypeInContext(context_)), 1, "");
    LLVMBuildRet(builder_.get(), result);
  }

  static constexpr auto progress_offset = offsetof(cpu12::detail::NativeContext, progress) +
                                          offsetof(cpu12::Progress, estimated_cycles);
  LLVMContextRef context_;
  LLVMValueRef function_;
  Builder builder_;
  const std::span<const BlockInstruction> instructions_;
  LLVMValueRef registers_, owner_, budget_;
  LLVMBasicBlockRef done_;
  LLVMValueRef d_ = nullptr, ccr_ = nullptr, pc_ = nullptr, cycles_ = nullptr;
  LLVMValueRef retired_ = nullptr, location_ = nullptr, deadline_ = nullptr;
  std::map<cpu12::Address, LLVMBasicBlockRef> blocks_;
};

}

LLVMValueRef
lift(LLVMModuleRef module, LLVMContextRef context, std::span<const BlockInstruction> instructions) {
  return FlowLifter(module, context, instructions).lift();
}

bool supports(const Instruction &instruction) noexcept {
  if (instruction.kind == Kind::nop || instruction.kind == Kind::branch) { return true; }
  if (instruction.addressing != Addressing::immediate) { return false; }
  if (instruction.kind == Kind::add || instruction.kind == Kind::subtract) {
    return instruction.word && instruction.target == WordTarget::d;
  }
  return instruction.kind == Kind::bit_xor && !instruction.word && !instruction.accumulator_b;
}
}
