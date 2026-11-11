// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_support.hpp"
#include "isa.hpp"
#include "jit/block.hpp"
#include "jit/image.hpp"
#include "jit/interrupt.hpp"
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/lifetime.hpp"
#include "jit/llvm/program.hpp"
#include "runtime.hpp"

#include "llvm-c/Analysis.h"
#include "llvm-c/BitReader.h"
#include "llvm-c/Core.h"
#include "llvm-c/Error.h"
#include "llvm-c/LLJIT.h"
#include "llvm-c/Orc.h"
#include "llvm-c/TargetMachine.h"
#include "llvm-c/Transforms/PassBuilder.h"
#include "llvm-c/Types.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpu12::test::image {
namespace {
template <class Ref, auto Dispose> using LlvmOwner = detail::llvm19::Owner<Ref, Dispose>;
constexpr Address input = 0x1234, vector = 0xfff0, idle = entry + 3;
constexpr unsigned retained_flags = 0xf1, retained_b = 0xa5;
constexpr unsigned frame_bytes = 9;
constexpr unsigned frames = 3;
constexpr Address irq_target = 0x4123;
constexpr Byte xirq_mask = 0x50;
constexpr std::uint64_t prior_cycles = 123;
constexpr Registers event_initial{.pc = entry, .d = 0x5aa5, .x = 0x1357, .y = 0x2468};
using Event = void (*)(Registers *, detail::NativeContext *) noexcept;
constexpr std::array addresses{entry, idle};
constexpr std::array requests{InterruptRequest{.vector = vector},
                              InterruptRequest{.vector = 0xfff4, .kind = InterruptKind::xirq}};

struct Semantic {
  std::vector<std::byte> bitcode;
  std::array<std::string, 2> entries;
  std::array<std::string, 7> events;
};

Semantic lower() {
  auto bytes = isa::program(
      isa::opcode<isa::Opcode::ldaa_extended>(),
      std::array<Byte, 2>{static_cast<Byte>(input >> byte_bits), static_cast<Byte>(input)},
      std::array<Byte, 2>{bra, backedge});
  const auto original = bytes;
  const std::array spans{CodeSpan{.address = entry, .words = bytes}};
  const std::array roots{entry};
  const std::array variants{CodeVariant{.id = 7, .code = spans, .entries = roots}};
  const detail::ImagePlan plan({.variants = variants});
  const auto memory = Memory::window({.first = entry, .bytes = bytes, .writable = false});
  const detail::llvm19::Program program(plan.variants().front().instructions,
                                        {.memory = {.source = memory, .bindings = memory},
                                         .prefix = "semantic_",
                                         .bindings = {},
                                         .output = detail::llvm19::ArtifactOutput::semantic,
                                         .interrupts = requests,
                                         .prepare_events = true,
                                         .callbacks = {}});
  require(
      !program.bitcode().empty() && program.object().empty() && program.native_bytes() == 0 &&
          program.find(entry) == nullptr && bytes == original,
      "Lowering retains verified pre-O3 IR without native emission, execution or source writes");
  Semantic result{
      .bitcode = {program.bitcode().begin(), program.bitcode().end()}, .entries = {}, .events = {}};
  for (std::size_t i = 0; i < addresses.size(); ++i) {
    result.entries[i] = program.symbol(addresses[i]);
  }
  std::size_t index = 0;
  for (const auto request : requests) {
    for (const auto frame : {detail::InterruptFrame::push,
                             detail::InterruptFrame::waiting,
                             detail::InterruptFrame::returned}) {
      const auto key = detail::event_key(request, frame);
      require(program.event(key) == nullptr, "Semantic IRQ entries have no executable address");
      result.events[index++] = program.event_symbol(key);
    }
  }
  result.events[index] = program.event_symbol(detail::stop_event_key);
  require(program.event(detail::stop_event_key) == nullptr,
          "Semantic STOP wake has no executable address");
  return result;
}

void checked(LLVMErrorRef error) {
  if (error == nullptr) { return; }
  const LlvmOwner<char *, LLVMDisposeErrorMessage> message(LLVMGetErrorMessage(error));
  throw std::runtime_error(message.get());
}

void dispose(LLVMOrcLLJITRef engine) noexcept {
  if (auto *error = LLVMOrcDisposeLLJIT(engine)) {
    LLVMConsumeError(error);
    std::terminate();
  }
}

using Engine = LlvmOwner<LLVMOrcLLJITRef, dispose>;

LlvmOwner<LLVMTargetMachineRef, LLVMDisposeTargetMachine> machine(LLVMModuleRef module) {
  LLVMTargetRef target = nullptr;
  char *error = nullptr;
  const auto failed = LLVMGetTargetFromTriple(LLVMGetTarget(module), &target, &error);
  const LlvmOwner<char *, LLVMDisposeMessage> message(error);
  require(failed == 0, "Resolve the semantic artifact's native target");
  const LlvmOwner<char *, LLVMDisposeMessage> cpu(LLVMGetHostCPUName());
  const auto features = detail::llvm19::native_features();
  return LlvmOwner<LLVMTargetMachineRef, LLVMDisposeTargetMachine>(
      LLVMCreateTargetMachine(target,
                              LLVMGetTarget(module),
                              cpu.get(),
                              features.c_str(),
                              LLVMCodeGenLevelAggressive,
                              LLVMRelocPIC,
                              LLVMCodeModelSmall));
}

LlvmOwner<LLVMModuleRef, LLVMDisposeModule> parse(LLVMContextRef context,
                                                  LLVMMemoryBufferRef buffer) {
  LLVMModuleRef raw = nullptr;
  require(LLVMParseBitcodeInContext2(context, buffer, &raw) == 0 && raw != nullptr,
          "The standalone consumer must parse the exported semantic bitcode");
  return LlvmOwner<LLVMModuleRef, LLVMDisposeModule>(raw);
}

Engine fuse(const Semantic &source) {
  const LlvmOwner<LLVMOrcThreadSafeContextRef, LLVMOrcDisposeThreadSafeContext> context(
      LLVMOrcCreateNewThreadSafeContext());
  auto *raw_context = LLVMOrcThreadSafeContextGetContext(context.get());
  const LlvmOwner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> bitcode(
      LLVMCreateMemoryBufferWithMemoryRangeCopy(
          reinterpret_cast<const char *>(source.bitcode.data()), source.bitcode.size(), "guest"));
  auto module = parse(raw_context, bitcode.get());
  for (const auto &symbol : source.entries) {
    auto *function = LLVMGetNamedFunction(module.get(), symbol.c_str());
    require(function != nullptr && LLVMIsDeclaration(function) == 0,
            "Each retained instruction symbol names a semantic function definition");
  }
  for (const auto &symbol : source.events) {
    auto *function = LLVMGetNamedFunction(module.get(), symbol.c_str());
    require(function != nullptr && LLVMIsDeclaration(function) == 0,
            "All three frames of both IRQ kinds and STOP wake remain in the same semantic module");
  }
  auto *read = LLVMGetNamedFunction(module.get(), "cpu12_read_byte");
  require(read != nullptr && LLVMIsDeclaration(read) != 0 && LLVMGetFirstUse(read) != nullptr,
          "Lowered guest reads still require the later consumer definition");
  char *error = nullptr;
  LLVMMemoryBufferRef raw = nullptr;
  const auto failed = LLVMCreateMemoryBufferWithContentsOfFile(CPU12_IMAGE_BINDINGS, &raw, &error);
  const LlvmOwner<char *, LLVMDisposeMessage> message(error);
  const LlvmOwner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> bindings(raw);
  require(failed == 0 && bindings != nullptr, "Read separately compiled C++ host binding IR");
  const auto *first = reinterpret_cast<const std::byte *>(LLVMGetBufferStart(bindings.get()));
  detail::llvm19::link_bindings(module.get(), {first, LLVMGetBufferSize(bindings.get())});
  const auto target = machine(module.get());
  require(target != nullptr, "Create the final joint optimizer's actual native target machine");
  const LlvmOwner<LLVMPassBuilderOptionsRef, LLVMDisposePassBuilderOptions> passes(
      LLVMCreatePassBuilderOptions());
  checked(LLVMRunPasses(module.get(), "default<O3>", target.get(), passes.get()));
  require(LLVMVerifyModule(module.get(), LLVMReturnStatusAction, nullptr) == 0,
          "The jointly optimized semantic and consumer IR must verify");
  read = LLVMGetNamedFunction(module.get(), "cpu12_read_byte");
  require(read == nullptr || LLVMGetFirstUse(read) == nullptr,
          "Joint O3 removes the opaque guest-to-host byte-read call boundary");
  LLVMOrcLLJITRef raw_engine = nullptr;
  checked(LLVMOrcCreateLLJIT(&raw_engine, nullptr));
  Engine engine(raw_engine);
  auto *safe = LLVMOrcCreateNewThreadSafeModule(module.release(), context.get());
  checked(
      LLVMOrcLLJITAddLLVMIRModule(engine.get(), LLVMOrcLLJITGetMainJITDylib(engine.get()), safe));
  return engine;
}

void execute(LLVMOrcLLJITRef engine, const Semantic &source) {
  detail::NativePage page{};
  detail::NativeIndex index{};
  index[entry >> detail::native_page_shift] = page.data();
  for (std::size_t i = 0; i < addresses.size(); ++i) {
    LLVMOrcExecutorAddress symbol = 0;
    checked(LLVMOrcLLJITLookup(engine, &symbol, source.entries[i].c_str()));
    page[addresses[i] % detail::native_page_entries] = std::bit_cast<const void *>(symbol);
  }
  std::array<Byte, address_bytes> ram{};
  for (unsigned value = 0; value <= byte_mask; ++value) {
    ram[input] = static_cast<Byte>(value);
    const auto memory = ram;
    for (unsigned flags = 0; flags <= byte_mask; ++flags) {
      for (unsigned budget = 0; budget <= 6; ++budget) {
        const Registers initial{.pc = entry,
                                .d = 0x5aa5,
                                .x = 0x1357,
                                .y = 0x2468,
                                .sp = 0x9000,
                                .ccr = static_cast<Byte>(flags)};
        detail::NativeContext owner{};
        owner.cpu.registers = initial;
        owner.io.owner = ram.data();
        auto expected = initial;
        if (budget != 0) {
          expected.pc = idle;
          expected.d = static_cast<Address>((value << byte_bits) | retained_b);
          expected.ccr = static_cast<Byte>((flags & retained_flags) | (value >= 0x80 ? 8U : 0U) |
                                           (value == 0 ? 4U : 0U));
        }
        const auto result = detail::enter_blocks(
            page[entry % detail::native_page_entries], owner.cpu.registers, owner, budget, index);
        require(result.retired == budget &&
                    owner.cpu == State{.registers = expected, .failure = {}} &&
                    owner.fault == Fault::none && !owner.yield_requested &&
                    owner.progress == Progress{.estimated_cycles = std::uint64_t{budget} * 3} &&
                    ram == memory,
                "Later joint compilation preserves every byte/CCR/prefix, complete state and RAM");
      }
    }
  }
}

struct EventCase {
  std::size_t request;
  unsigned frame;
  Address sp;
  Byte flags;
};

void interrupt_case(const Event code, const EventCase point) {
  const auto [request, frame, sp, flags] = point;
  std::array<Byte, address_bytes> ram{};
  ram[requests[request].vector] = static_cast<Byte>(irq_target >> byte_bits);
  ram[requests[request].vector + 1] = static_cast<Byte>(irq_target);
  auto memory = ram;
  detail::NativeContext owner{};
  owner.io.owner = ram.data();
  owner.cpu = {.registers = event_initial,
               .failure = {},
               .execution = frame == 1 ? ExecutionState::waiting : ExecutionState::running,
               .irq_delay = 1,
               .returned_frame = frame == 2};
  owner.cpu.registers.sp = sp;
  owner.cpu.registers.ccr = flags;
  owner.progress.estimated_cycles = prior_cycles;
  auto expected = owner.cpu;
  if (frame == 0) {
    const std::array<Byte, frame_bytes> saved{flags, 0xa5, 0x5a, 0x13, 0x57, 0x24, 0x68, 0x40, 0};
    for (unsigned i = 0; i < saved.size(); ++i) {
      memory[static_cast<Address>(int{sp} - int{frame_bytes} + static_cast<int>(i))] = saved[i];
    }
  }
  if (frame != 1) { expected.registers.sp = static_cast<Address>(int{sp} - int{frame_bytes}); }
  expected.registers.pc = irq_target;
  expected.registers.ccr |= request == 0 ? 0x10 : xirq_mask;
  expected.execution = ExecutionState::running;
  expected.irq_delay = 0;
  expected.returned_frame = false;
  code(&owner.cpu.registers, &owner);
  require(owner.cpu == expected && owner.progress == Progress{.estimated_cycles = prior_cycles} &&
              owner.fault == Fault::none && !owner.yield_requested && ram == memory,
          "Later host fusion preserves all IRQ frame variants, read-before-write and wrap");
}

void events(LLVMOrcLLJITRef engine, const Semantic &source) {
  std::array<Event, 7> code{};
  for (std::size_t i = 0; i < code.size(); ++i) {
    LLVMOrcExecutorAddress symbol = 0;
    checked(LLVMOrcLLJITLookup(engine, &symbol, source.events[i].c_str()));
    code[i] = std::bit_cast<Event>(symbol);
  }
  constexpr std::array<Address, 5> stacks{0, 1, 8, 0x9000, 0xfff9};
  for (std::size_t request = 0; request < requests.size(); ++request) {
    for (unsigned frame = 0; frame < frames; ++frame) {
      for (const auto sp : stacks) {
        for (unsigned flags = 0; flags <= byte_mask; ++flags) {
          interrupt_case(
              code[(request * frames) + frame],
              {.request = request, .frame = frame, .sp = sp, .flags = static_cast<Byte>(flags)});
        }
      }
    }
  }
  for (const auto sp : stacks) {
    detail::NativeContext owner{};
    owner.cpu = {.registers = {.pc = entry, .sp = sp},
                 .failure = {},
                 .execution = ExecutionState::stopped,
                 .irq_delay = 1,
                 .returned_frame = true};
    auto expected = owner.cpu;
    expected.registers.sp = static_cast<Address>(sp + frame_bytes);
    expected.execution = ExecutionState::running;
    code.back()(&owner.cpu.registers, &owner);
    require(owner.cpu == expected && owner.progress == Progress{} && owner.fault == Fault::none,
            "Later host fusion retains the STOP wake ABI, wrapping SP and unrelated state");
  }
}
}

void semantic_contract() {
  const auto source = lower();
  const auto engine = fuse(source);
  execute(engine.get(), source);
  events(engine.get(), source);
}
}
