// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/lift.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/flow.hpp"
#include "jit/llvm/indexed.hpp"
#include "jit/llvm/memory.hpp"
#include "jit/llvm/registers.hpp"
#include "jit/llvm/stack.hpp"
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

class FlowLifter {
public:
  FlowLifter(LLVMValueRef function,
             const Flow &flow,
             const std::span<const BlockInstruction> instructions,
             const Memory &memory)
      : context_(LLVMGetModuleContext(LLVMGetGlobalParent(function))),
        function_(function),
        flow_(flow),
        builder_(context_, LLVMAppendBasicBlockInContext(context_, function_, "entry")),
        instructions_(instructions),
        memory_(memory),
        registers_(LLVMGetParam(function_, 0)),
        owner_(LLVMGetParam(function_, 1)),
        budget_(LLVMGetParam(function_, 2)),
        done_(LLVMAppendBasicBlockInContext(context_, function_, "done")),
        dispatch_(LLVMAppendBasicBlockInContext(context_, function_, "dispatch")),
        outward_(LLVMAppendBasicBlockInContext(context_, function_, "outward")) {}

  void lift() {
    initialize();
    for (const auto &operation : instructions_) {
      blocks_.emplace(operation.pc,
                      LLVMAppendBasicBlockInContext(
                          context_, function_, ("pc" + std::to_string(operation.pc)).c_str()));
    }
    dispatch(done_);
    LLVMPositionBuilderAtEnd(builder_.get(), dispatch_);
    dispatch(outward_);
    for (const auto &operation : instructions_) { emit(operation); }
    for (const auto &[pc, block] : bridges_) { transfer(block, flow_.target(pc)); }
    transfer(outward_, flow_.dispatcher());
    finish();
  }

private:
  void dispatch(LLVMBasicBlockRef missing) {
    auto *select = LLVMBuildSwitch(builder_.get(),
                                   load(pc_, builder_.getInt16Ty()),
                                   missing,
                                   static_cast<unsigned>(blocks_.size()));
    for (const auto &[pc, block] : blocks_) { LLVMAddCase(select, builder_.getInt16(pc), block); }
  }

  void transfer(LLVMBasicBlockRef block, LLVMValueRef target) {
    LLVMPositionBuilderAtEnd(builder_.get(), block);
    publish();
    std::array arguments{registers_,
                         owner_,
                         budget_,
                         load(retired_, builder_.getInt32Ty()),
                         load(location_, builder_.getInt64Ty())};
    flow_.transfer(builder_, target, arguments);
  }

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
    builder_.CreateStore(LLVMGetParam(function_, 4), location_);
    builder_.CreateStore(LLVMGetParam(function_, 3), retired_);
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

  LLVMValueRef branch_condition(const Instruction &instruction, Values &state) {
    if (instruction.kind == Kind::loop) { return loop_condition(instruction, state); }
    auto *ccr = state.ccr;
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

  LLVMValueRef loop_condition(const Instruction &instruction, Values &state) {
    auto *value = register_value(builder_, state, instruction);
    if (instruction.operation != RegisterOp::test) {
      constexpr unsigned byte_mask = 0xff;
      constexpr unsigned word_mask = 0xffff;
      const auto delta = instruction.operation == RegisterOp::increment ? 1U : word_mask;
      value = builder_.CreateAnd(builder_.CreateAdd(value, builder_.getInt32(delta)),
                                 builder_.getInt32(instruction.word ? word_mask : byte_mask));
      register_value(builder_, state, instruction, value);
    }
    return instruction.condition != 0 ? builder_.CreateICmpNE(value, builder_.getInt32(0))
                                      : builder_.CreateICmpEQ(value, builder_.getInt32(0));
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
    if (instruction.kind == Kind::branch || instruction.kind == Kind::loop) {
      taken = branch_condition(instruction, state);
      destination = builder_.CreateSelect(
          taken,
          builder_.getInt16(static_cast<cpu12::Address>(next + instruction.operand)),
          destination);
      cost =
          builder_.CreateSelect(taken, builder_.getInt64(timing.base + timing.taken_extra), cost);
    } else if (instruction.kind == Kind::call || instruction.kind == Kind::jump) {
      destination = control_value(operation, state);
    } else if (frame_operand(instruction)) {
      destination = instruction.kind == Kind::return_interrupt ? return_frame(operation, state)
                                                               : wait_frame(operation, state, cost);
    } else if (instruction.kind == Kind::effective_address) {
      auto *value = effective_address(builder_, state, operation);
      commit_index(builder_, state, instruction);
      register_value(builder_, state, {.word = true, .target = instruction.target}, value);
    } else if (stack_operand(instruction)) {
      auto *previous = state.ccr;
      destination = stack_value(operation, state);
      if (mask_operation(instruction)) { interrupt_masks(previous, state.ccr, false); }
    } else if (instruction.kind == Kind::memory_unary) {
      memory_unary(operation, state);
    } else if (instruction.kind == Kind::register_operation) {
      if (instruction.operation == RegisterOp::decimal_adjust) {
        validate_decimal(operation, state);
      }
      auto *previous = state.ccr;
      lower_register(builder_, state, instruction);
      if (mask_operation(instruction)) { interrupt_masks(previous, state.ccr); }
    } else if (instruction.kind == Kind::transfer) {
      auto *previous = state.ccr;
      lower_transfer(builder_, state, instruction);
      if (transfer_masks(instruction)) {
        constexpr unsigned ignored = 8;
        interrupt_masks(previous, state.ccr, (instruction.operand & ~ignored) == 2);
      }
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
    if (memory_operand(instruction) || stack_operand(instruction) || frame_operand(instruction) ||
        mask_operation(instruction)) {
      auto *flow_block = LLVMAppendBasicBlockInContext(context_, function_, "resume");
      LLVMBuildCondBr(
          builder_.get(), load(boundary_, LLVMInt1TypeInContext(context_)), done_, flow_block);
      LLVMPositionBuilderAtEnd(builder_.get(), flow_block);
    }
    continue_flow(operation, taken);
  }

  void continue_flow(const BlockInstruction &operation, LLVMValueRef taken) {
    const auto flow = successors(operation);
    if (flow.indirect) {
      LLVMBuildBr(builder_.get(), dispatch_);
    } else if (operation.instruction.kind == Kind::call) {
      LLVMBuildBr(builder_.get(), target(flow.direct[flow.count - 1]));
    } else if (flow.count == 1) {
      LLVMBuildBr(builder_.get(), target(flow.direct[0]));
    } else if (flow.count == 2) {
      LLVMBuildCondBr(builder_.get(), taken, target(flow.direct[1]), target(flow.direct[0]));
    } else {
      throw std::logic_error("CPU12 LLVM lowering requires a prepared control-flow successor");
    }
  }

  LLVMValueRef stack_value(const BlockInstruction &operation, Values &state) {
    const auto &instruction = operation.instruction;
    const bool push = instruction.kind == Kind::push;
    const int width = instruction.word ? 2 : 1;
    const Instruction access{.kind = push ? Kind::store : Kind::load,
                             .addressing = Addressing::indexed,
                             .word = instruction.word};
    auto *source = push ? stack_register(builder_, state, instruction.stack_target) : nullptr;
    auto *result =
        transfer(operation,
                 access,
                 {.address = stack_address(builder_, state, push ? -width : 0), .value = source});
    state.sp = stack_address(builder_, state, push ? -width : width);
    if (instruction.kind == Kind::return_subroutine) {
      return builder_.CreateTrunc(result, builder_.getInt16Ty());
    }
    if (!push) { stack_register(builder_, state, instruction.stack_target, result); }
    return builder_.getInt16(static_cast<Address>(operation.pc + instruction.length));
  }

  LLVMValueRef return_frame(const BlockInstruction &operation, Values &state) {
    const Instruction byte{.kind = Kind::load, .addressing = Addressing::indexed};
    const Instruction word{.kind = Kind::load, .addressing = Addressing::indexed, .word = true};
    const auto read = [&](const int offset, const Instruction access) {
      return transfer(operation, access, {.address = stack_address(builder_, state, offset)});
    };
    auto *ccr = read(0, byte);
    auto *d = read(1, word);
    auto *x = read(3, word);
    auto *pc = read(7, word);
    auto *y = read(5, word);
    stack_register(builder_, state, StackTarget::ccr, ccr);
    state.d = swap_frame_word(builder_, d);
    state.x = x;
    state.y = y;
    state.sp = stack_address(builder_, state, 9);
    builder_.CreateStore(
        builder_.getInt8(0),
        address(owner_, offsetof(NativeContext, cpu) + offsetof(State, irq_delay)));
    builder_.CreateStore(
        builder_.getInt8(1),
        address(owner_, offsetof(NativeContext, cpu) + offsetof(State, returned_frame)));
    builder_.CreateStore(LLVMConstInt(LLVMInt1TypeInContext(context_), 1, 0), boundary_);
    return builder_.CreateTrunc(pc, builder_.getInt16Ty());
  }

  void push_frame(const BlockInstruction &operation, const Values &state) {
    const Instruction word{.kind = Kind::store, .addressing = Addressing::indexed, .word = true};
    const Instruction byte{.kind = Kind::store, .addressing = Addressing::indexed};
    const auto write = [&](const int offset, LLVMValueRef value, const Instruction access) {
      static_cast<void>(transfer(
          operation, access, {.address = stack_address(builder_, state, offset), .value = value}));
    };
    write(-2,
          builder_.getInt32(static_cast<Address>(operation.pc + operation.instruction.length)),
          word);
    write(-4, state.y, word);
    write(-6, state.x, word);
    write(-8, swap_frame_word(builder_, state.d), word);
    write(-9, state.ccr, byte);
  }

  LLVMValueRef wait_frame(const BlockInstruction &operation, Values &state, LLVMValueRef &cost) {
    const bool stop = operation.instruction.kind == Kind::stop;
    auto *previous = LLVMGetInsertBlock(builder_.get());
    auto *resume = LLVMAppendBasicBlockInContext(context_, function_, "wait_resume");
    if (stop) {
      constexpr unsigned stop_mask = 0x80;
      auto *masked = builder_.CreateICmpNE(
          builder_.CreateAnd(state.ccr, builder_.getInt32(stop_mask)), builder_.getInt32(0));
      auto *body = LLVMAppendBasicBlockInContext(context_, function_, "stop_frame");
      LLVMBuildCondBr(builder_.get(), masked, resume, body);
      LLVMPositionBuilderAtEnd(builder_.get(), body);
    }
    push_frame(operation, state);
    auto *stacked_sp = stack_address(builder_, state, -9);
    builder_.CreateStore(
        builder_.getInt8(
            static_cast<Byte>(stop ? ExecutionState::stopped : ExecutionState::waiting)),
        address(owner_, offsetof(NativeContext, cpu) + offsetof(State, execution)));
    builder_.CreateStore(LLVMConstInt(LLVMInt1TypeInContext(context_), 1, 0), boundary_);
    auto *stacked = LLVMGetInsertBlock(builder_.get());
    LLVMBuildBr(builder_.get(), resume);
    LLVMPositionBuilderAtEnd(builder_.get(), resume);
    if (stop) {
      const auto timing = instruction_timing(operation.instruction);
      auto *sp = LLVMBuildPhi(builder_.get(), builder_.getInt32Ty(), "stop_sp");
      std::array pointers{state.sp, stacked_sp};
      std::array blocks{previous, stacked};
      LLVMAddIncoming(sp, pointers.data(), blocks.data(), blocks.size());
      auto *cycles = LLVMBuildPhi(builder_.get(), builder_.getInt64Ty(), "stop_cycles");
      std::array values{cost, builder_.getInt64(timing.base + timing.taken_extra)};
      LLVMAddIncoming(cycles, values.data(), blocks.data(), blocks.size());
      state.sp = sp;
      cost = cycles;
    } else {
      state.sp = stacked_sp;
    }
    return builder_.getInt16(static_cast<Address>(operation.pc + operation.instruction.length));
  }

  LLVMValueRef control_value(const BlockInstruction &operation, Values &state) {
    const auto &instruction = operation.instruction;
    const auto next = static_cast<Address>(operation.pc + instruction.length);
    auto *destination = instruction.addressing == Addressing::relative
                            ? builder_.getInt32(static_cast<Address>(next + instruction.operand))
                            : effective_address(builder_, state, operation);
    if (instruction.addressing == Addressing::indexed && instruction.indexed.indirect) {
      const Instruction pointer{
          .kind = Kind::load, .addressing = Addressing::indexed, .word = true};
      destination = transfer(operation, pointer, {.address = destination});
    }
    auto adjusted = state;
    commit_index(builder_, adjusted, instruction);
    if (instruction.kind == Kind::call) {
      const Instruction access{
          .kind = Kind::store, .addressing = Addressing::indexed, .word = true};
      static_cast<void>(transfer(
          operation,
          access,
          {.address = stack_address(builder_, adjusted, -2), .value = builder_.getInt32(next)}));
      adjusted.sp = stack_address(builder_, adjusted, -2);
    }
    state = adjusted;
    return builder_.CreateTrunc(destination, builder_.getInt16Ty());
  }

  static bool mask_operation(const Instruction &instruction) {
    return (instruction.kind == Kind::register_operation &&
            (instruction.operation == RegisterOp::ccr_and ||
             instruction.operation == RegisterOp::ccr_or)) ||
           (instruction.kind == Kind::pull && instruction.stack_target == StackTarget::ccr) ||
           transfer_masks(instruction);
  }

  void interrupt_masks(LLVMValueRef previous, LLVMValueRef current, const bool defer = true) {
    constexpr unsigned irq = 0x10;
    constexpr unsigned masks = 0x50;
    auto *unmasked = builder_.CreateAnd(
        builder_.CreateICmpNE(builder_.CreateAnd(previous, builder_.getInt32(irq)),
                              builder_.getInt32(0)),
        builder_.CreateICmpEQ(builder_.CreateAnd(current, builder_.getInt32(irq)),
                              builder_.getInt32(0)));
    constexpr auto delay_offset = offsetof(NativeContext, cpu) + offsetof(State, irq_delay);
    if (defer) {
      builder_.CreateStore(
          builder_.CreateSelect(unmasked,
                                builder_.getInt8(1),
                                load(address(owner_, delay_offset), builder_.getInt8Ty())),
          address(owner_, delay_offset));
    }
    auto *changed = builder_.CreateICmpNE(
        builder_.CreateAnd(builder_.CreateXor(previous, current), builder_.getInt32(masks)),
        builder_.getInt32(0));
    builder_.CreateStore(
        builder_.CreateOr(load(boundary_, LLVMInt1TypeInContext(context_)), changed), boundary_);
  }

  void validate_decimal(const BlockInstruction &operation, const Values &state) {
    const auto domain = decimal_domain(builder_, state);
    auto *accepted = LLVMAppendBasicBlockInContext(context_, function_, "decimal_valid");
    auto *refused = LLVMAppendBasicBlockInContext(context_, function_, "decimal_invalid");
    LLVMBuildCondBr(builder_.get(), domain.valid, accepted, refused);
    LLVMPositionBuilderAtEnd(builder_.get(), refused);
    builder_.CreateStore(builder_.getInt8(static_cast<std::uint8_t>(Fault::unsupported_state)),
                         address(owner_, offsetof(NativeContext, fault)));
    builder_.CreateStore(builder_.getInt16(0), address(owner_, offsetof(NativeContext, address)));
    builder_.CreateStore(builder_.CreateTrunc(domain.input, builder_.getInt16Ty()),
                         address(owner_, offsetof(NativeContext, value)));
    builder_.CreateStore(builder_.getInt8(0),
                         address(owner_, offsetof(NativeContext, access_size)));
    const auto location =
        static_cast<std::uint64_t>(operation.pc) |
        (static_cast<std::uint64_t>(operation.instruction.opcode) << native_opcode_shift);
    builder_.CreateStore(builder_.getInt64(location), location_);
    builder_.CreateStore(LLVMConstInt(LLVMInt1TypeInContext(context_), 1, 0), boundary_);
    LLVMBuildBr(builder_.get(), done_);
    LLVMPositionBuilderAtEnd(builder_.get(), accepted);
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

  void memory_unary(const BlockInstruction &operation, Values &state) {
    const auto &instruction = operation.instruction;
    auto *at = effective_address(builder_, state, operation);
    if (instruction.addressing == Addressing::indexed && instruction.indexed.indirect) {
      const Instruction pointer{
          .kind = Kind::load, .addressing = Addressing::indexed, .word = true};
      at = transfer(operation, pointer, {.address = at});
    }
    const Instruction read{.kind = Kind::load, .addressing = Addressing::indexed};
    auto *source = instruction.operation == RegisterOp::clear
                       ? builder_.getInt32(0)
                       : transfer(operation, read, {.address = at});
    const auto result = lower_unary(builder_, source, state.ccr, instruction);
    if (instruction.operation != RegisterOp::test) {
      const Instruction write{.kind = Kind::store, .addressing = Addressing::indexed};
      static_cast<void>(transfer(operation, write, {.address = at, .value = result.value}));
    }
    commit_index(builder_, state, instruction);
    state.ccr = result.ccr;
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

  [[nodiscard]] LLVMBasicBlockRef target(Address address) {
    const auto found = blocks_.find(address);
    if (found != blocks_.end()) { return found->second; }
    if (flow_.target(address) == nullptr) { return done_; }
    const auto [bridge, inserted] = bridges_.try_emplace(address, nullptr);
    if (inserted) {
      bridge->second = LLVMAppendBasicBlockInContext(context_, function_, "transfer");
    }
    return bridge->second;
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
  const Flow &flow_;
  Builder builder_;
  const std::span<const BlockInstruction> instructions_;
  const Memory &memory_;
  LLVMValueRef registers_, owner_, budget_;
  LLVMBasicBlockRef done_, dispatch_, outward_;
  LLVMValueRef d_ = nullptr, ccr_ = nullptr, pc_ = nullptr, cycles_ = nullptr;
  LLVMValueRef x_ = nullptr, y_ = nullptr;
  LLVMValueRef sp_ = nullptr;
  LLVMValueRef retired_ = nullptr, location_ = nullptr, deadline_ = nullptr;
  LLVMValueRef boundary_ = nullptr;
  std::map<cpu12::Address, LLVMBasicBlockRef> blocks_, bridges_;
};

}

void lift(LLVMValueRef function,
          const Flow &flow,
          std::span<const BlockInstruction> instructions,
          const Memory &memory) {
  FlowLifter(function, flow, instructions, memory).lift();
}

bool supports(const Instruction &instruction) noexcept {
  if (instruction.kind == Kind::nop || instruction.kind == Kind::branch ||
      instruction.kind == Kind::loop || instruction.kind == Kind::effective_address) {
    return true;
  }
  if (instruction.kind == Kind::register_operation || instruction.kind == Kind::transfer) {
    return true;
  }
  if (stack_operand(instruction) || frame_operand(instruction) || instruction.kind == Kind::jump) {
    return true;
  }
  if (instruction.addressing != Addressing::immediate && !memory_operand(instruction)) {
    return false;
  }
  if (instruction.kind == Kind::store) { return memory_operand(instruction); }
  if (instruction.kind == Kind::memory_unary) { return memory_operand(instruction); }
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
