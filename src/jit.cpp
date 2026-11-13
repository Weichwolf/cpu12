// SPDX-License-Identifier: GPL-3.0-only
// The typed SysV bridge and W^X allocation pattern are adapted from the independent
// Weichwolf/dsp56300 project (GPL-3.0-only). No foreign emulator is used.
#include "jit.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "immutable.hpp"
#include "instruction.hpp"
#include "jit/arena.hpp"
#include "jit/block.hpp"
#include "jit/emitter.hpp"
#include "jit/image.hpp"
#include "jit/interrupt.hpp"
#include "jit/llvm/image.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/program.hpp"
#include "jit/llvm/regions.hpp"
#include "jit/preparation.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
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
bool same_region(const Ram &left, const Ram &right) noexcept {
  return left.first == right.first && left.bytes.data() == right.bytes.data() &&
         left.bytes.size() == right.bytes.size() && left.known.data() == right.known.data() &&
         left.known.size() == right.known.size() && left.writable == right.writable &&
         left.identity == right.identity;
}

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
  Instruction instruction{};
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
  std::bitset<address_bytes> unavailable;
  std::bitset<address_bytes> prepared;
  std::shared_ptr<const llvm19::ImageProgram> image;
  const CodeTargets *transitions = nullptr;
  std::uint64_t variant = 0;

  void install(Address address, const void *entry) {
    auto &page = native_regions[address >> native_page_shift];
    if (page == nullptr) {
      auto &owned = pages[address >> native_page_shift];
      owned = std::make_unique<NativePage>();
      page = owned->data();
    }
    auto &slot = (*pages[address >> native_page_shift])[address & page_mask];
    if (slot == nullptr) { ++native_entries; }
    slot = entry;
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

  [[nodiscard]] bool matches(const Memory &requested) const noexcept {
    const auto regions = requested.regions();
    return regions.size() == grants.size() && std::ranges::equal(regions, grants, same_region);
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

  ExecutableArena arena;
  std::vector<std::unique_ptr<llvm19::Program>> programs;
  std::vector<std::unique_ptr<Mapping>> mappings;
  std::unique_ptr<Mapping> image_binding;
  Mapping *current = nullptr;
  Statistics statistics;
  std::optional<Transfer> pending;

  void completed_transfer(const Address pc, NativeContext &context) {
    const auto *targets = current->transitions;
    if (targets == nullptr) { return; }
    const auto target = targets->find(pc);
    if (target == targets->end()) { return; }
    pending = Transfer{.image = current->image, .variants = target->second, .entry = pc};
    context.yield_requested = true;
  }

  [[nodiscard]] PreparationReport prepared_image(const std::span<const Address> entries) const {
    if (pending.has_value()) {
      const auto pc = pending->entry;
      return {.unresolved_entries = 1,
              .failure = {.fault = Fault::unprepared_execution, .pc = pc, .address = pc}};
    }
    PreparationReport report;
    std::bitset<address_bytes> unresolved;
    if (current->image) {
      report.prepared_entries = current->image->instructions(current->variant).size();
    }
    for (const auto address : entries) {
      if (!current->image || current->image->program(current->variant).find(address) == nullptr) {
        if (report.failure.fault == Fault::none) {
          report.failure = {
              .fault = Fault::unprepared_execution, .pc = address, .address = address};
        }
        unresolved[address] = true;
      }
    }
    if (!current->image && entries.empty()) { report.failure.fault = Fault::unprepared_execution; }
    report.unresolved_entries = unresolved.count();
    return report;
  }

  [[nodiscard]] std::vector<BlockInstruction>
  reuse_prepared(Mapping &mapping, const std::span<const BlockInstruction> instructions) {
    std::vector<BlockInstruction> remaining;
    for (const auto &region : llvm19::regions(instructions)) {
      const void *entry = nullptr;
      for (const auto &program : programs) {
        entry = program->find(region, mapping.memory);
        if (entry != nullptr) { break; }
      }
      if (entry == nullptr) {
        remaining.insert(remaining.end(), region.begin(), region.end());
        continue;
      }
      for (const auto &operation : region) {
        mapping.install(operation.pc, entry);
        mapping.prepared[operation.pc] = true;
      }
    }
    return remaining;
  }

  // Keep translation-only allocation and stack state off the retained-entry path.
  [[nodiscard, gnu::cold, gnu::noinline]] std::optional<NativeResult>
  run_cold(const std::uint32_t budget, Registers &registers, NativeContext &context) {
    auto &mapping = *current;
    const auto instructions = parse_block(mapping.memory, registers.pc);
    if (instructions.empty()) {
      mapping.unavailable[registers.pc] = true;
      return std::nullopt;
    }
    const auto *entry = arena.append(emit_block(instructions, mapping.memory));
    mapping.install(registers.pc, entry);
    ++statistics.compiled_entries;
    const auto result = mapping.run(entry, registers, context, budget);
    return result;
  }
};

Jit::Jit(const Memory &ram) : impl_(std::make_unique<Impl>()) {
  set_memory(ram);
}

const MemoryBinding *Jit::bindings() const noexcept {
  return impl_->current->bindings.data();
}

void Jit::set_memory(const Memory &memory) {
  if (impl_->image_binding) {
    auto mapping = std::make_unique<Mapping>(memory);
    impl_->image_binding = std::move(mapping);
    impl_->current = impl_->image_binding.get();
    return;
  }
  for (const auto &mapping : impl_->mappings) {
    if (mapping->matches(memory)) {
      impl_->current = mapping.get();
      return;
    }
  }
  auto mapping = std::make_unique<Mapping>(memory);
  auto *selected = mapping.get();
  impl_->mappings.push_back(std::move(mapping));
  impl_->current = selected;
}

void Jit::install(std::shared_ptr<const llvm19::ImageProgram> image,
                  const std::uint64_t variant,
                  const std::span<const InterruptRequest> interrupts) {
  const auto &pending = impl_->pending;
  if (pending.has_value() &&
      (pending->image != image ||
       std::ranges::find(pending->variants, variant) == pending->variants.end())) {
    throw std::invalid_argument("Pending CPU12 transition requires its declared image and variant");
  }
  image->validate_binding(variant, impl_->current->memory);
  auto mapping = std::make_unique<Mapping>(impl_->current->memory);
  mapping->image = std::move(image);
  mapping->variant = variant;
  mapping->transitions = &mapping->image->transitions(variant);
  mapping->native_regions = mapping->image->index(variant);
  mapping->native_entries = static_cast<unsigned>(mapping->image->instructions(variant).size());
  constexpr std::array frames{
      InterruptFrame::push, InterruptFrame::waiting, InterruptFrame::returned};
  for (const auto request : interrupts) {
    for (const auto frame : frames) {
      const auto *code = mapping->image->program(variant).event(event_key(request, frame));
      if (code == nullptr) { throw std::invalid_argument("CPU12 image has no prepared IRQ entry"); }
      mapping->blocks.emplace(event_key(request, frame), Entry{.code = code});
    }
  }
  const auto *wake = mapping->image->program(variant).event(stop_event_key);
  if (wake == nullptr) {
    throw std::invalid_argument("CPU12 image has no prepared STOP-wake entry");
  }
  mapping->blocks.emplace(stop_event_key, Entry{.code = wake});
  impl_->image_binding = std::move(mapping);
  impl_->current = impl_->image_binding.get();
  impl_->pending.reset();
}

bool Jit::requires_image() const noexcept {
  return impl_->image_binding != nullptr;
}

bool Jit::transition_pending() const noexcept {
  return impl_->pending.has_value();
}

void Jit::cancel_transition() noexcept {
  impl_->pending.reset();
}

Fault Jit::validate_instruction(const Address pc, const std::span<const Byte> bytes) const {
  const auto &mapping = *impl_->current;
  if (!mapping.image) { return Fault::unprepared_execution; }
  const auto *instruction = mapping.image->program(mapping.variant).instruction(pc);
  if (instruction == nullptr) { return Fault::unprepared_execution; }
  if (instruction->length != bytes.size()) { return Fault::invalid_input; }
  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    const auto source = immutable_bytes(mapping.memory, static_cast<Address>(pc + offset));
    if (source.empty() || source.front() != bytes[offset]) { return Fault::invalid_input; }
  }
  return Fault::none;
}

PreparationReport Jit::prepare(const std::span<const Address> established_entries) {
  auto *const mapping = impl_->current;
  if (requires_image()) { return impl_->prepared_image(established_entries); }
  auto graph = preparation_graph(mapping->memory, established_entries);
  PreparationReport report{.failure = graph.failure};
  std::bitset<address_bytes> unresolved;
  for (const auto address : graph.unresolved) { unresolved[address] = true; }
  std::vector<BlockInstruction> instructions;
  for (const auto &operation : graph.instructions) {
    if (!llvm19::supports(operation.instruction)) {
      unresolved[operation.pc] = true;
      continue;
    }
    ++report.prepared_entries;
    if (!mapping->prepared[operation.pc]) { instructions.push_back(operation); }
  }
  report.unresolved_entries = unresolved.count();
  instructions = impl_->reuse_prepared(*mapping, instructions);
  if (instructions.empty()) { return report; }
  auto program = std::make_unique<llvm19::Program>(instructions, mapping->memory);
  if (impl_->current != mapping) {
    throw std::logic_error("CPU12 binding changed during preparation");
  }
  const auto *entries = program.get();
  report.native_bytes = program->native_bytes();
  impl_->programs.push_back(std::move(program));
  ++impl_->statistics.compiled_entries;
  for (const auto &operation : instructions) {
    mapping->install(operation.pc, entries->entry(operation.pc));
    mapping->prepared[operation.pc] = true;
  }
  return report;
}

const Memory &Jit::memory() const noexcept {
  return impl_->current->memory;
}

Jit::~Jit() = default;

bool Jit::execute(const std::span<const std::uint8_t> bytes,
                  Registers &registers,
                  NativeContext &context) {
  if (requires_image()) { return false; }
  constexpr unsigned length_shift = 48;
  constexpr unsigned opcode_shift = 40;
  constexpr unsigned postbyte_shift = 32;
  constexpr unsigned high_shift = 24;
  constexpr unsigned low_shift = 16;
  constexpr unsigned byte_shift = 8;
  static_assert(maximum_capture_bytes * byte_shift <= length_shift);
  // Include every raw byte and the exact length. Trailing masks/displacements
  // and malformed padded captures must never hit another native entry.
  auto key = (static_cast<std::uint64_t>(bytes.size()) << length_shift) |
             (static_cast<std::uint64_t>(bytes[0]) << opcode_shift);
  if (bytes.size() > 1) { key |= static_cast<std::uint64_t>(bytes[1]) << postbyte_shift; }
  if (bytes.size() > 2) { key |= static_cast<std::uint64_t>(bytes[2]) << high_shift; }
  if (bytes.size() > 3) { key |= static_cast<std::uint64_t>(bytes[3]) << low_shift; }
  if (bytes.size() > 4) { key |= static_cast<std::uint64_t>(bytes[4]) << byte_shift; }
  if (bytes.size() > 5) { key |= bytes[5]; }
  auto found = impl_->current->blocks.find(key);
  if (found == impl_->current->blocks.end()) {
    const auto instruction = decode(bytes);
    if (!instruction) { return false; }
    found =
        impl_->current->blocks
            .emplace(key,
                     Entry{.code = impl_->arena.append(emit(*instruction, impl_->current->memory)),
                           .instruction = *instruction})
            .first;
    ++impl_->statistics.compiled_entries;
  }
  enter(found->second.code, registers, context);
  if (context.fault == Fault::none) {
    const auto &instruction = found->second.instruction;
    const auto timing = instruction_timing(instruction);
    const bool extra =
        instruction.kind == Kind::branch
            ? branch_taken(instruction, registers.ccr)
            : instruction.kind == Kind::stop && context.cpu.execution == ExecutionState::stopped;
    context.progress.estimated_cycles += timing.base + (extra ? timing.taken_extra : 0U);
    if (context.cpu.irq_delay != 0) { --context.cpu.irq_delay; }
    if (context.cpu.returned_frame) {
      constexpr unsigned rti = 0x0b;
      constexpr auto rti_key =
          (std::uint64_t{1} << length_shift) | (std::uint64_t{rti} << opcode_shift);
      if (key != rti_key) { context.cpu.returned_frame = false; }
    }
  }
  return true;
}

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
  const auto *instruction =
      returned ? mapping.image->program(mapping.variant).instruction(registers.pc) : nullptr;
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
    impl_->completed_transfer(registers.pc, context);
  }
  return result;
}

std::optional<NativeResult>
Jit::run_block(const std::uint32_t budget, Registers &registers, NativeContext &context) {
  if (requires_image()) { return run_prepared(budget, registers, context); }
  if (context.cpu.returned_frame) { return std::nullopt; }
  auto &mapping = *impl_->current;
  const auto address = unsigned{registers.pc};
  auto &page = mapping.native_regions[address >> native_page_shift];
  const auto *entry = page != nullptr ? page[address & page_mask] : nullptr;
  if (context.cpu.irq_delay != 0) {
    if (entry == nullptr || !mapping.prepared[address]) { return std::nullopt; }
    const auto previous = registers.ccr;
    const auto result = mapping.run(entry, registers, context, std::min(budget, 1U));
    constexpr unsigned irq = 0x10;
    const bool unmasked = (previous & irq) != 0 && (registers.ccr & irq) == 0;
    if (result.retired != 0 && context.fault == Fault::none && context.cpu.irq_delay != 0 &&
        !unmasked) {
      --context.cpu.irq_delay;
    }
    return result;
  }
  if (entry == nullptr) {
    if (mapping.unavailable[address]) { return std::nullopt; }
    return impl_->run_cold(budget, registers, context);
  }
  const auto result = mapping.run(entry, registers, context, budget);
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
    if (requires_image()) {
      unprepared(context, request.vector);
      return;
    }
    found = impl_->current->blocks
                .emplace(key,
                         Entry{.code = impl_->arena.append(
                                   emit_interrupt(request, impl_->current->memory, frame))})
                .first;
    ++impl_->statistics.compiled_entries;
  }
  enter(found->second.code, registers, context);
  if (context.fault == Fault::none) { impl_->completed_transfer(registers.pc, context); }
}

void Jit::resume_stop(Registers &registers, NativeContext &context) {
  auto found = impl_->current->blocks.find(stop_event_key);
  if (found == impl_->current->blocks.end()) {
    if (requires_image()) {
      unprepared(context, registers.pc);
      return;
    }
    found = impl_->current->blocks
                .emplace(stop_event_key, Entry{.code = impl_->arena.append(emit_stop_wake())})
                .first;
    ++impl_->statistics.compiled_entries;
  }
  enter(found->second.code, registers, context);
}

Statistics Jit::statistics() const noexcept {
  return impl_->statistics;
}
}
