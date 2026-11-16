// SPDX-License-Identifier: GPL-3.0-only
// The typed SysV bridge and W^X allocation pattern are adapted from the independent
// Weichwolf/dsp56300 project (GPL-3.0-only). No foreign emulator is used.
#include "jit.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/interrupt.hpp"
#include "jit/llvm/image.hpp"
#include "jit/llvm/program.hpp"
#include "jit/prefixes.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cpu12::detail {
namespace {
#if defined(__x86_64__) && defined(__linux__)
extern "C" void cpu12_enter_generated(const void *, Registers *, NativeContext *) noexcept;
asm(".pushsection .text\n"
    ".globl cpu12_enter_generated\n"
    ".hidden cpu12_enter_generated\n"
    ".type cpu12_enter_generated,@function\n"
    "cpu12_enter_generated:\n"
    "endbr64\n"
    "mov %rdi,%rax\n"
    "mov %rsi,%rdi\n"
    "mov %rdx,%rsi\n"
    "jmp *%rax\n"
    ".size cpu12_enter_generated,.-cpu12_enter_generated\n"
    ".popsection\n");
#elif defined(__aarch64__) && (defined(__APPLE__) || defined(__linux__))
extern "C" void cpu12_enter_generated(const void *, Registers *, NativeContext *) noexcept;
#endif
void enter(const void *entry, Registers &registers, NativeContext &context) noexcept {
#if (defined(__x86_64__) && defined(__linux__)) ||                                                 \
    (defined(__aarch64__) && (defined(__APPLE__) || defined(__linux__)))
  cpu12_enter_generated(entry, &registers, &context);
#else
  static_cast<void>(entry);
  static_cast<void>(registers);
  static_cast<void>(context);
#endif
}

}

namespace {

constexpr unsigned page_mask = static_cast<unsigned>(native_page_entries - 1);

void unprepared(NativeContext &context, const Address address) noexcept {
  context.fault = Fault::unprepared_execution;
  context.address = address;
  context.value = 0;
  context.access_size = 0;
  context.read_mask = 0;
}

struct Entry {
  const void *code = nullptr;
};

struct Mapping {
  explicit Mapping(const Memory &requested)
      : grants(requested.regions().begin(), requested.regions().end()), memory(Memory::external()) {
    if (grants.size() == 1) {
      memory = Memory::window(grants[0]);
    } else if (!grants.empty()) {
      memory = Memory::mapped(grants);
    }
    for (const auto &grant : grants) {
      bindings.push_back({.bytes = grant.bytes.data(), .known = grant.known.data()});
    }
  }

  std::vector<Ram> grants;
  std::vector<MemoryBinding> bindings;
  Memory memory;
  std::unordered_map<std::uint64_t, Entry> blocks;
  std::array<std::unique_ptr<NativePage>, native_page_entries> pages;
  NativeIndex native_regions{};
  unsigned native_entries = 0;
  std::shared_ptr<const llvm19::ImageProgram> image;
  std::vector<std::uint64_t> variants;
  std::vector<InterruptRequest> interrupts;
  GuardedPrefixes guarded, local_reads, local_effects;

  [[nodiscard]] const void *entry(const Address pc) const noexcept {
    const auto *page = native_regions[pc >> native_page_shift];
    return page != nullptr ? page[pc & page_mask] : nullptr;
  }

  [[nodiscard]] std::optional<std::uint64_t> source(const Address pc) const {
    if (!image) { return std::nullopt; }
    for (const auto id : variants) {
      const auto *page = image->index(id)[pc >> native_page_shift];
      if (page != nullptr && page[pc & page_mask] != nullptr) { return id; }
    }
    return std::nullopt;
  }

  [[nodiscard]] const Instruction *instruction(const Address pc) const {
    const auto id = source(pc);
    return id ? image->program(*id).instruction(pc) : nullptr;
  }

  [[nodiscard]] std::vector<BlockInstruction> instructions() const {
    std::vector<BlockInstruction> result;
    result.reserve(native_entries);
    for (const auto id : variants) {
      const auto code = image->instructions(id);
      result.insert(result.end(), code.begin(), code.end());
    }
    return result;
  }

  void mount(const std::uint64_t id) {
    const auto &index = image->index(id);
    for (std::size_t page = 0; page < index.size(); ++page) {
      if (index[page] == nullptr) { continue; }
      auto &target = native_regions[page];
      if (target == nullptr) {
        target = index[page];
        continue;
      }
      auto &owned = pages[page];
      if (!owned) {
        owned = std::make_unique<NativePage>();
        std::copy_n(target, native_page_entries, owned->begin());
        target = owned->data();
      }
      for (std::size_t offset = 0; offset < native_page_entries; ++offset) {
        if (index[page][offset] != nullptr) { (*owned)[offset] = index[page][offset]; }
      }
    }
    native_entries += static_cast<unsigned>(image->instructions(id).size());
  }

  [[nodiscard]] NativeResult run(const void *entry,
                                 Registers &registers,
                                 NativeContext &context,
                                 const std::uint32_t budget) const noexcept {
#if defined(__aarch64__) && (defined(__APPLE__) || defined(__linux__))
    if (native_entries == 1) { return enter_block(entry, registers, context, budget); }
#endif
    return enter_blocks(entry, registers, context, budget, native_regions);
  }
};
}

class Jit::Impl {
public:
  struct Transfer {
    std::shared_ptr<const llvm19::ImageProgram> image;
    std::span<const std::uint64_t> variants;
    Address entry;
  };

  std::unique_ptr<Mapping> current;
  std::optional<Transfer> pending;

  void completed_transfer(const ExitLocation from, const Address pc, NativeContext &context) {
    if (!current->image || current->entry(pc) != nullptr) { return; }
    const auto id = current->source(from.pc).value_or(current->variants.front());
    const auto &targets = current->image->transitions(id);
    const auto target = targets.find(pc);
    if (target == targets.end()) { return; }
    pending = Transfer{.image = current->image, .variants = target->second, .entry = pc};
    context.yield_requested = true;
  }
};

Jit::Jit(const Memory &ram) : impl_(std::make_unique<Impl>()) {
  set_memory(ram);
}

const MemoryBinding *Jit::bindings() const noexcept {
  return impl_->current->bindings.data();
}

void Jit::set_memory(const Memory &memory) {
  impl_->current = std::make_unique<Mapping>(memory);
}

void Jit::install(std::shared_ptr<const llvm19::ImageProgram> image,
                  const std::span<const std::uint64_t> variants,
                  const std::span<const InterruptRequest> interrupts) {
  const auto &pending = impl_->pending;
  if (pending &&
      (pending->image != image || !std::ranges::any_of(variants, [&](const auto id) {
         return std::ranges::find(pending->variants, id) != pending->variants.end() &&
                image->index(id)[pending->entry >> native_page_shift] != nullptr &&
                image->index(id)[pending->entry >> native_page_shift][pending->entry & page_mask] !=
                    nullptr;
       }))) {
    throw std::invalid_argument("Pending CPU12 transition requires its declared image and variant");
  }
  image->validate_composition(variants, impl_->current->memory);
  auto mapping = std::make_unique<Mapping>(impl_->current->memory);
  mapping->image = std::move(image);
  mapping->variants.assign(variants.begin(), variants.end());
  mapping->interrupts.assign(interrupts.begin(), interrupts.end());
  for (const auto id : variants) { mapping->mount(id); }
  constexpr std::array frames{
      InterruptFrame::push, InterruptFrame::waiting, InterruptFrame::returned};
  for (const auto request : interrupts) {
    for (const auto frame : frames) {
      const auto *code = mapping->image->program(variants.front()).event(event_key(request, frame));
      if (code == nullptr) { throw std::invalid_argument("CPU12 image has no prepared IRQ entry"); }
      mapping->blocks.emplace(event_key(request, frame), Entry{.code = code});
    }
  }
  const auto *wake = mapping->image->program(variants.front()).event(stop_event_key);
  if (wake == nullptr) {
    throw std::invalid_argument("CPU12 image has no prepared STOP-wake entry");
  }
  mapping->blocks.emplace(stop_event_key, Entry{.code = wake});
  mapping->guarded = GuardedPrefixes(mapping->instructions(), mapping->memory);
  impl_->current = std::move(mapping);
  impl_->pending.reset();
}

void Jit::isolate_accesses(const std::span<const LocalAccess> accesses) {
  auto &mapping = *impl_->current;
  if (!mapping.image || impl_->pending) {
    throw std::logic_error("CPU12 isolation requires installed code without a pending transfer");
  }
  const auto instructions = mapping.instructions();
  auto effects = accesses.empty() ? GuardedPrefixes{}
                                  : GuardedPrefixes(instructions, mapping.memory, accesses);
  if (std::ranges::all_of(
          accesses, [](const auto access) { return access.role == LocalAccess::Role::read; })) {
    mapping.local_reads = std::move(effects);
    mapping.local_effects = {};
    return;
  }
  std::vector<LocalAccess> reads;
  for (auto access : accesses) {
    if (access.role == LocalAccess::Role::write) { continue; }
    access.role = LocalAccess::Role::read;
    reads.push_back(access);
  }
  auto reader =
      reads.empty() ? GuardedPrefixes{} : GuardedPrefixes(instructions, mapping.memory, reads);
  mapping.local_reads = std::move(reader);
  mapping.local_effects = std::move(effects);
}

bool Jit::transition_pending() const noexcept {
  return impl_->pending.has_value();
}

std::uint64_t
Jit::foreground_prefix(const State &state, const bool local, const ChannelAccess access) const {
  if (impl_->pending) { return 0; }
  const auto &mapping = *impl_->current;
  const auto pc = state.registers.pc;
  const auto source = mapping.source(pc);
  if (!source) { return 0; }
  const auto *instruction = mapping.image->program(*source).instruction(pc);
  if (instruction != nullptr &&
      ((instruction->kind == Kind::wait && private_frame(mapping.memory, state.registers.sp)) ||
       (instruction->kind == Kind::return_interrupt &&
        private_return_frame(mapping.memory, state.registers.sp)))) {
    return instruction_timing(*instruction).base;
  }
  return std::max({mapping.image->foreground_prefix(*source, pc),
                   mapping.guarded.prefix(pc, state.registers),
                   local ? mapping.local_reads.prefix(pc, state.registers) : 0,
                   local ? mapping.local_effects.prefix(pc, state.registers) : 0,
                   local && instruction != nullptr
                       ? private_operation_prefix(*instruction, mapping.memory, state.registers)
                       : 0,
                   local && instruction != nullptr
                       ? isolated_operation_prefix(*instruction, state.registers, access)
                       : 0});
}

std::uint64_t Jit::event_prefix(const State &state,
                                const bool local,
                                const std::optional<std::span<const InterruptRequest>> incoming,
                                const ChannelAccess access) const {
  if (impl_->pending || !impl_->current->image) { return 0; }
  const auto &mapping = *impl_->current;
  const auto requests = incoming.value_or(mapping.interrupts);
  if (std::ranges::any_of(requests, [&](const auto request) {
        return !std::ranges::any_of(mapping.interrupts, [&](const auto installed) {
          return request.vector == installed.vector && request.kind == installed.kind;
        });
      })) {
    return 0;
  }
  const PrefixLookup lookup{
      .owner = &mapping,
      .find = [](const void *owner, const Address pc) -> std::optional<std::uint64_t> {
        const auto &installed = *static_cast<const Mapping *>(owner);
        const auto source = installed.source(pc);
        return source ? std::optional{installed.image->foreground_prefix(*source, pc)}
                      : std::nullopt;
      }};
  auto foreground = state.execution == ExecutionState::waiting
                        ? std::numeric_limits<std::uint64_t>::max()
                        : foreground_prefix(state, local, access);
  if (state.returned_frame) {
    const auto *next = mapping.instruction(state.registers.pc);
    if (next == nullptr) { return 0; }
    foreground = std::min(foreground, std::uint64_t{instruction_timing(*next).base});
  }
  return std::min(foreground, interrupt_entry_prefix(state, mapping.memory, lookup, requests));
}

void Jit::cancel_transition() noexcept {
  impl_->pending.reset();
}

const Memory &Jit::memory() const noexcept {
  return impl_->current->memory;
}

Jit::~Jit() = default;

NativeResult
Jit::run_prepared(const std::uint32_t budget, Registers &registers, NativeContext &context) {
  if (impl_->pending) {
    context.yield_requested = true;
    return {.location = {.pc = registers.pc}};
  }
  auto &mapping = *impl_->current;
  const auto address = unsigned{registers.pc};
  const auto *page = mapping.native_regions[address >> native_page_shift];
  const auto *entry = page != nullptr ? page[address & page_mask] : nullptr;
  if (!mapping.image || entry == nullptr) {
    unprepared(context, registers.pc);
    return {.location = {.pc = registers.pc}};
  }
  const bool returned = context.cpu.returned_frame;
  const auto *instruction = returned ? mapping.instruction(registers.pc) : nullptr;
  const auto previous = registers.ccr;
  const auto delay = context.cpu.irq_delay;
  const auto grant = delay != 0 || returned ? std::min(budget, 1U) : budget;
  const auto result = mapping.run(entry, registers, context, grant);
  if (result.retired != 0 && context.fault == Fault::none) {
    constexpr unsigned irq = 0x10;
    const bool unmasked = (previous & irq) != 0 && (registers.ccr & irq) == 0;
    if (delay != 0 && context.cpu.irq_delay != 0 && !unmasked) { --context.cpu.irq_delay; }
    if (instruction != nullptr && instruction->kind != Kind::return_interrupt) {
      context.cpu.returned_frame = false;
    }
    impl_->completed_transfer(result.location, registers.pc, context);
  }
  return result;
}

void Jit::interrupt(const InterruptRequest request, Registers &registers, NativeContext &context) {
  auto frame = InterruptFrame::push;
  if (context.cpu.execution != ExecutionState::running) {
    frame = InterruptFrame::waiting;
  } else if (context.cpu.returned_frame) {
    frame = InterruptFrame::returned;
  }
  const auto key = event_key(request, frame);
  auto found = impl_->current->blocks.find(key);
  if (found == impl_->current->blocks.end()) {
    unprepared(context, request.vector);
    return;
  }
  const ExitLocation from{.pc = registers.pc};
  enter(found->second.code, registers, context);
  if (context.fault == Fault::none) { impl_->completed_transfer(from, registers.pc, context); }
}

void Jit::resume_stop(Registers &registers, NativeContext &context) {
  auto found = impl_->current->blocks.find(stop_event_key);
  if (found == impl_->current->blocks.end()) {
    unprepared(context, registers.pc);
    return;
  }
  enter(found->second.code, registers, context);
}

}
