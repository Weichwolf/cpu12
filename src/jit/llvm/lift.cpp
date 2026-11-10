// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/lift.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/indexed.hpp"
#include "jit/llvm/memory.hpp"
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

LLVMValueRef make_function(LLVMModuleRef module, LLVMContextRef context, const char *name) {
  auto *pointer = LLVMPointerTypeInContext(context, 0);
  std::array fields{LLVMInt64TypeInContext(context), LLVMInt64TypeInContext(context)};
  auto *result = LLVMStructTypeInContext(context, fields.data(), fields.size(), 0);
  std::array parameters{pointer, pointer, LLVMInt32TypeInContext(context)};
  auto *type = LLVMFunctionType(result, parameters.data(), parameters.size(), 0);
  return LLVMAddFunction(module, name, type);
}

class FlowLifter {
public:
  FlowLifter(LLVMModuleRef module,
             LLVMContextRef context,
             const std::span<const BlockInstruction> instructions,
             const Memory &memory,
             const char *name)
      : context_(context),
        function_(make_function(module, context, name)),
        builder_(context, LLVMAppendBasicBlockInContext(context, function_, "entry")),
        instructions_(instructions),
        memory_(memory),
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
    x_ = LLVMBuildAlloca(builder_.get(), builder_.getInt32Ty(), "x");
    y_ = LLVMBuildAlloca(builder_.get(), builder_.getInt32Ty(), "y");
    sp_ = LLVMBuildAlloca(builder_.get(), builder_.getInt32Ty(), "sp");
    ccr_ = LLVMBuildAlloca(builder_.get(), builder_.getInt32Ty(), "ccr");
    pc_ = LLVMBuildAlloca(builder_.get(), builder_.getInt16Ty(), "pc");
    cycles_ = LLVMBuildAlloca(builder_.get(), LLVMInt64TypeInContext(context_), "cycles");
    retired_ = LLVMBuildAlloca(builder_.get(), builder_.getInt32Ty(), "retired");
    location_ = LLVMBuildAlloca(builder_.get(), LLVMInt64TypeInContext(context_), "location");
    boundary_ = LLVMBuildAlloca(builder_.get(), LLVMInt1TypeInContext(context_), "boundary");
    builder_.CreateStore(LLVMConstInt(LLVMInt1TypeInContext(context_), 0, 0), boundary_);
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
    builder_.CreateStore(
        builder_.CreateZExt(field(offsetof(cpu12::Registers, x), builder_.getInt16Ty()),
                            builder_.getInt32Ty()),
        x_);
    builder_.CreateStore(
        builder_.CreateZExt(field(offsetof(cpu12::Registers, y), builder_.getInt16Ty()),
                            builder_.getInt32Ty()),
        y_);
    builder_.CreateStore(
        builder_.CreateZExt(field(offsetof(cpu12::Registers, sp), builder_.getInt16Ty()),
                            builder_.getInt32Ty()),
        sp_);
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
    Values state{.d = load(d_, builder_.getInt32Ty()),
                 .x = load(x_, builder_.getInt32Ty()),
                 .y = load(y_, builder_.getInt32Ty()),
                 .sp = load(sp_, builder_.getInt32Ty()),
                 .ccr = load(ccr_, builder_.getInt32Ty())};
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
      auto *operand = memory_operand(instruction) ? memory_value(operation, state) : nullptr;
      lower(builder_, state, instruction, operand);
    }
    builder_.CreateStore(state.d, d_);
    builder_.CreateStore(state.x, x_);
    builder_.CreateStore(state.y, y_);
    builder_.CreateStore(state.sp, sp_);
    builder_.CreateStore(state.ccr, ccr_);
    builder_.CreateStore(destination, pc_);
    builder_.CreateStore(builder_.CreateAdd(elapsed, cost), cycles_);
    builder_.CreateStore(builder_.CreateAdd(completed, builder_.getInt32(1)), retired_);
    const auto location =
        static_cast<std::uint64_t>(operation.pc) |
        (static_cast<std::uint64_t>(instruction.opcode) << cpu12::detail::native_opcode_shift);
    builder_.CreateStore(builder_.getInt64(location), location_);
    if (memory_operand(instruction)) {
      auto *flow_block = LLVMAppendBasicBlockInContext(context_, function_, "resume");
      LLVMBuildCondBr(
          builder_.get(), load(boundary_, LLVMInt1TypeInContext(context_)), done_, flow_block);
      LLVMPositionBuilderAtEnd(builder_.get(), flow_block);
    }
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

  LLVMValueRef memory_value(const BlockInstruction &operation, Values &state) {
    const auto &instruction = operation.instruction;
    auto *address = effective_address(builder_, state, operation);
    if (instruction.addressing == Addressing::indexed && instruction.indexed.indirect) {
      auto pointer = instruction;
      pointer.kind = Kind::load;
      pointer.word = true;
      address = transfer(operation, pointer, {.address = address});
    }
    auto adjusted = state;
    commit_index(builder_, adjusted, instruction);
    auto *value =
        instruction.kind == Kind::store ? register_value(builder_, adjusted, instruction) : nullptr;
    auto *result = transfer(operation, instruction, {.address = address, .value = value});
    commit_index(builder_, state, instruction);
    return result;
  }

  LLVMValueRef transfer(const BlockInstruction &operation,
                        const Instruction instruction,
                        const AccessValues values) {
    MemoryAccess access(builder_, owner_, instruction, values);
    access.direct(memory_);
    publish();
    const auto location =
        static_cast<std::uint64_t>(operation.pc) |
        (static_cast<std::uint64_t>(operation.instruction.opcode) << native_opcode_shift);
    builder_.CreateStore(builder_.getInt64(location), location_);
    builder_.CreateStore(LLVMConstInt(LLVMInt1TypeInContext(context_), 1, 0), boundary_);
    auto *result = access.external();
    auto *failed =
        instruction.kind == Kind::store
            ? builder_.CreateICmpEQ(result, builder_.getInt32(0))
            : builder_.CreateICmpEQ(
                  result, builder_.getInt32(instruction.word ? failed_word_read : failed_read));
    auto *accepted = LLVMAppendBasicBlockInContext(context_, function_, "accepted");
    LLVMBuildCondBr(builder_.get(), failed, done_, accepted);
    LLVMPositionBuilderAtEnd(builder_.get(), accepted);
    auto *yield =
        load(address(owner_, offsetof(NativeContext, yield_requested)), builder_.getInt8Ty());
    builder_.CreateStore(builder_.CreateICmpNE(yield, builder_.getInt8(0)), boundary_);
    return access.join(result);
  }

  [[nodiscard]] LLVMBasicBlockRef target(Address address) const {
    const auto found = blocks_.find(address);
    return found == blocks_.end() ? done_ : found->second;
  }

  void publish() {
    builder_.CreateStore(
        builder_.CreateTrunc(load(d_, builder_.getInt32Ty()), builder_.getInt16Ty()),
        address(registers_, offsetof(cpu12::Registers, d)));
    builder_.CreateStore(
        builder_.CreateTrunc(load(ccr_, builder_.getInt32Ty()), builder_.getInt8Ty()),
        address(registers_, offsetof(cpu12::Registers, ccr)));
    builder_.CreateStore(
        builder_.CreateTrunc(load(x_, builder_.getInt32Ty()), builder_.getInt16Ty()),
        address(registers_, offsetof(cpu12::Registers, x)));
    builder_.CreateStore(
        builder_.CreateTrunc(load(y_, builder_.getInt32Ty()), builder_.getInt16Ty()),
        address(registers_, offsetof(cpu12::Registers, y)));
    builder_.CreateStore(
        builder_.CreateTrunc(load(sp_, builder_.getInt32Ty()), builder_.getInt16Ty()),
        address(registers_, offsetof(cpu12::Registers, sp)));
    builder_.CreateStore(load(pc_, builder_.getInt16Ty()),
                         address(registers_, offsetof(cpu12::Registers, pc)));
    builder_.CreateStore(load(cycles_, LLVMInt64TypeInContext(context_)),
                         address(owner_, progress_offset));
  }

  void finish() {
    LLVMPositionBuilderAtEnd(builder_.get(), done_);
    publish();
    auto *result = LLVMGetUndef(LLVMGetReturnType(LLVMGlobalGetValueType(function_)));
    result = LLVMBuildInsertValue(builder_.get(),
                                  result,
                                  builder_.CreateZExt(load(retired_, builder_.getInt32Ty()),
                                                      LLVMInt64TypeInContext(context_)),
                                  0,
                                  "");
    auto *boundary = builder_.CreateSelect(load(boundary_, LLVMInt1TypeInContext(context_)),
                                           builder_.getInt64(native_boundary_mask),
                                           builder_.getInt64(0));
    auto *location = builder_.CreateOr(load(location_, LLVMInt64TypeInContext(context_)), boundary);
    result = LLVMBuildInsertValue(builder_.get(), result, location, 1, "");
    LLVMBuildRet(builder_.get(), result);
  }

  static constexpr auto progress_offset = offsetof(cpu12::detail::NativeContext, progress) +
                                          offsetof(cpu12::Progress, estimated_cycles);
  LLVMContextRef context_;
  LLVMValueRef function_;
  Builder builder_;
  const std::span<const BlockInstruction> instructions_;
  const Memory &memory_;
  LLVMValueRef registers_, owner_, budget_;
  LLVMBasicBlockRef done_;
  LLVMValueRef d_ = nullptr, ccr_ = nullptr, pc_ = nullptr, cycles_ = nullptr;
  LLVMValueRef x_ = nullptr, y_ = nullptr;
  LLVMValueRef sp_ = nullptr;
  LLVMValueRef retired_ = nullptr, location_ = nullptr, deadline_ = nullptr;
  LLVMValueRef boundary_ = nullptr;
  std::map<cpu12::Address, LLVMBasicBlockRef> blocks_;
};

}

LLVMValueRef lift(LLVMModuleRef module,
                  LLVMContextRef context,
                  std::span<const BlockInstruction> instructions,
                  const Memory &memory,
                  const char *name) {
  return FlowLifter(module, context, instructions, memory, name).lift();
}

bool supports(const Instruction &instruction) noexcept {
  if (instruction.kind == Kind::nop || instruction.kind == Kind::branch) { return true; }
  if (instruction.kind == Kind::register_operation) {
    return (instruction.operation == RegisterOp::index_increment ||
            instruction.operation == RegisterOp::index_decrement) &&
           (instruction.target == WordTarget::x || instruction.target == WordTarget::y);
  }
  if (instruction.addressing != Addressing::immediate && !memory_operand(instruction)) {
    return false;
  }
  if (instruction.kind == Kind::store) { return memory_operand(instruction); }
  if (instruction.kind == Kind::load || instruction.kind == Kind::compare) { return true; }
  if (instruction.kind == Kind::add || instruction.kind == Kind::subtract) {
    return !instruction.word || instruction.target == WordTarget::d;
  }
  return !instruction.word &&
         (instruction.kind == Kind::add_carry || instruction.kind == Kind::subtract_carry ||
          instruction.kind == Kind::bit_and || instruction.kind == Kind::bit_or ||
          instruction.kind == Kind::bit_xor || instruction.kind == Kind::bit_test);
}
}
