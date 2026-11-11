// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/events.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/interrupt.hpp"
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/builder.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/memory.hpp"
#include "jit/llvm/stack.hpp"
#include "runtime.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>

namespace cpu12::detail::llvm19 {
namespace {
class EventLifter {
public:
  EventLifter(LLVMValueRef function, const Memory &memory)
      : function_(function),
        builder_(LLVMGetModuleContext(LLVMGetGlobalParent(function)), block("entry")),
        memory_(memory),
        registers_(LLVMGetParam(function, 0)),
        owner_(LLVMGetParam(function, 1)),
        done_(block("done")) {}

  void interrupt(const InterruptRequest request, const InterruptFrame frame) {
    const auto state = values();
    const Instruction read{.kind = Kind::load,
                           .addressing = Addressing::extended,
                           .address = request.vector,
                           .word = true};
    auto *target = transfer(read, {.address = builder_.getInt32(request.vector)});
    if (frame == InterruptFrame::push) {
      push_frame(builder_,
                 state,
                 field(offsetof(Registers, pc), builder_.getInt16Ty()),
                 [&](const Instruction access, const AccessValues access_values) {
                   return transfer(access, access_values);
                 });
    }
    if (frame != InterruptFrame::waiting) {
      store(offsetof(Registers, sp), stack_address(builder_, state, -9), builder_.getInt16Ty());
    }
    const unsigned mask = request.kind == InterruptKind::xirq ? 0x50 : 0x10;
    store(offsetof(Registers, ccr),
          builder_.CreateOr(state.ccr, builder_.getInt32(mask)),
          builder_.getInt8Ty());
    store(offsetof(Registers, pc), target, builder_.getInt16Ty());
    control(offsetof(State, execution), static_cast<Byte>(ExecutionState::running));
    control(offsetof(State, irq_delay), 0);
    control(offsetof(State, returned_frame), 0);
    finish();
  }

  void wake() {
    store(offsetof(Registers, sp), stack_address(builder_, values(), 9), builder_.getInt16Ty());
    control(offsetof(State, execution), static_cast<Byte>(ExecutionState::running));
    finish();
  }

private:
  LLVMBasicBlockRef block(const char *name) {
    return LLVMAppendBasicBlockInContext(
        LLVMGetModuleContext(LLVMGetGlobalParent(function_)), function_, name);
  }

  LLVMValueRef address(LLVMValueRef base, const std::size_t offset) {
    return builder_.CreateGEP(builder_.getInt8Ty(), base, builder_.getInt64(offset));
  }

  LLVMValueRef field(const std::size_t offset, LLVMTypeRef type) {
    return builder_.CreateZExt(builder_.CreateLoad(type, address(registers_, offset)),
                               builder_.getInt32Ty());
  }

  Values values() {
    return {.d = field(offsetof(Registers, d), builder_.getInt16Ty()),
            .x = field(offsetof(Registers, x), builder_.getInt16Ty()),
            .y = field(offsetof(Registers, y), builder_.getInt16Ty()),
            .sp = field(offsetof(Registers, sp), builder_.getInt16Ty()),
            .ccr = field(offsetof(Registers, ccr), builder_.getInt8Ty())};
  }

  void store(const std::size_t offset, LLVMValueRef value, LLVMTypeRef type) {
    builder_.CreateStore(builder_.CreateTrunc(value, type), address(registers_, offset));
  }

  void control(const std::size_t offset, const Byte value) {
    builder_.CreateStore(builder_.getInt8(value),
                         address(owner_, offsetof(NativeContext, cpu) + offset));
  }

  LLVMValueRef transfer(const Instruction instruction, const AccessValues values) {
    MemoryAccess access(builder_, owner_, instruction, values);
    access.direct(memory_);
    auto *result = access.external();
    auto *failed = builder_.CreateICmpEQ(
        result, builder_.getInt32(instruction.kind == Kind::store ? 0 : failed_word_read));
    auto *accepted = block("accepted");
    LLVMBuildCondBr(builder_.get(), failed, done_, accepted);
    LLVMPositionBuilderAtEnd(builder_.get(), accepted);
    return access.join(result);
  }

  void finish() {
    LLVMBuildBr(builder_.get(), done_);
    LLVMPositionBuilderAtEnd(builder_.get(), done_);
    builder_.CreateRetVoid();
  }

  LLVMValueRef function_;
  Builder builder_;
  const Memory &memory_;
  LLVMValueRef registers_, owner_;
  LLVMBasicBlockRef done_;
};
}

std::map<std::uint64_t, std::string> lift_events(LLVMModuleRef module,
                                                 const Memory &memory,
                                                 const std::string &prefix,
                                                 const std::span<const InterruptRequest> interrupts,
                                                 const bool native) {
  std::map<std::uint64_t, std::string> symbols;
  auto *context = LLVMGetModuleContext(module);
  auto *pointer = LLVMPointerTypeInContext(context, 0);
  std::array parameters{pointer, pointer};
  auto *type =
      LLVMFunctionType(LLVMVoidTypeInContext(context), parameters.data(), parameters.size(), 0);
  const auto function = [&](const std::uint64_t key) {
    const auto name = prefix + "event_" + std::to_string(key);
    auto *entry = LLVMAddFunction(module, name.c_str(), type);
    if (native) { native_attributes(entry); }
    symbols.emplace(key, name);
    return entry;
  };
  constexpr std::array frames{
      InterruptFrame::push, InterruptFrame::waiting, InterruptFrame::returned};
  for (const auto request : interrupts) {
    for (const auto frame : frames) {
      EventLifter(function(event_key(request, frame)), memory).interrupt(request, frame);
    }
  }
  EventLifter(function(stop_event_key), memory).wake();
  return symbols;
}
}
