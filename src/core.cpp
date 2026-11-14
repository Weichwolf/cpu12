// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"

#include "cpu12/memory.hpp"
#include "cycles.hpp"
#include "fetch.hpp"
#include "image.hpp"
#include "jit.hpp"
#include "jit/block.hpp"
#include "jit/llvm/module.hpp"
#include "memory_access.hpp"
#include "runtime.hpp"
#include "semantic.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#if defined(__x86_64__) && defined(__linux__)
#include <cpuid.h>
#endif

namespace cpu12 {
namespace {
class DeadlineScope {
public:
  DeadlineScope(bool &active, detail::NativeContext &context, const std::uint64_t deadline)
      : active_(active), context_(context) {
    active_ = true;
    context_.cycle_deadline = deadline;
  }

  ~DeadlineScope() {
    active_ = false;
    context_.cycle_deadline = std::numeric_limits<std::uint64_t>::max();
  }

  DeadlineScope(const DeadlineScope &) = delete;
  DeadlineScope &operator=(const DeadlineScope &) = delete;

private:
  bool &active_;
  detail::NativeContext &context_;
};

bool native_supported() noexcept {
#if defined(__x86_64__) && defined(__linux__)
  if (std::endian::native != std::endian::little) { return false; }
  constexpr unsigned extended_features = 0x80000001;
  constexpr unsigned lahf_lm = 1;
  unsigned eax = 0;
  unsigned ebx = 0;
  unsigned ecx = 0;
  unsigned edx = 0;
  return __get_cpuid(extended_features, &eax, &ebx, &ecx, &edx) != 0 && (ecx & lahf_lm) != 0;
#elif defined(__aarch64__) && (defined(__APPLE__) || defined(__linux__))
  return std::endian::native == std::endian::little;
#else
  return false;
#endif
}

std::uint32_t
reset_vector(const Memory &ram, detail::NativeContext &context, const Address vector) noexcept {
  const auto *high = detail::known_byte(ram, vector);
  const auto *low = detail::known_byte(ram, static_cast<Address>(vector + 1U));
  if (high == nullptr || low == nullptr) { return detail::read_word(&context, vector); }
  constexpr unsigned byte_bits = 8;
  return (static_cast<std::uint32_t>(*high) << byte_bits) | *low;
}
}

class Core::Impl {
public:
  Impl(const Memory &memory, const Io io) : context{.io = io}, jit(memory) {
    context.memory_bindings = jit.bindings();
    context.memory_grants = jit.memory().regions();
  }

  const bool supported = native_supported();
  detail::NativeContext context;
  detail::Jit jit;
  bool active = false;
  bool deadline_active = false;
  bool prefix_admitted = false;

  [[nodiscard]] bool prefix_ready() const noexcept {
    return !active && supported && prefix_admitted && context.cpu.failure.fault == Fault::none;
  }

  void admit_prefix_memory() noexcept {
    const auto owner = std::bit_cast<std::uintptr_t>(this);
    const auto outside = [&](const std::span<const Byte> bytes) {
      if (bytes.empty()) { return true; }
      const auto first = std::bit_cast<std::uintptr_t>(bytes.data());
      return first <= owner ? owner - first >= bytes.size() : first - owner >= sizeof(*this);
    };
    prefix_admitted = std::ranges::all_of(jit.memory().regions(), [&](const Ram &region) {
      return outside(region.bytes) && outside(region.known);
    });
  }

  void observe_interrupt_boundary(const Byte previous_masks, const Byte previous_delay) noexcept {
    constexpr unsigned masks = 0x50;
    if (previous_masks != (context.cpu.registers.ccr & masks) ||
        (previous_delay != 0 && context.cpu.irq_delay == 0) || context.cpu.returned_frame) {
      context.yield_requested = true;
    }
  }

  RunResult failed(RunResult result) noexcept {
    result.state = context.cpu.execution;
    result.fault = context.fault;
    result.address = context.address;
    result.value = context.value;
    result.access_size = context.access_size;
    if (context.access_size == 1 &&
        (context.fault == Fault::memory_read || context.fault == Fault::instruction_fetch)) {
      result.read_mask = context.read_mask;
    }
    context.cpu.failure = result;
    return result;
  }

  EventResult failed_event(EventResult result) noexcept {
    const auto failure = failed({.pc = result.pc});
    result.fault = failure.fault;
    result.address = failure.address;
    result.value = failure.value;
    result.access_size = failure.access_size;
    return result;
  }

  RunResult execute(const std::span<const std::uint8_t> bytes) {
    RunResult result{.pc = context.cpu.registers.pc, .opcode = bytes[0]};
    if (!jit.execute(bytes, context.cpu.registers, context)) {
      result.fault = Fault::unsupported_instruction;
    } else if (context.fault != Fault::none) {
      return failed(result);
    } else {
      result.retired = 1;
      result.state = context.cpu.execution;
    }
    return result;
  }

  RunResult block_result(const detail::NativeResult native) {
    RunResult result{.retired = native.retired,
                     .state = context.cpu.execution,
                     .pc = native.location.pc,
                     .opcode = native.location.opcode};
    if (context.fault != Fault::none) {
      result.retired = 0;
      result = failed(result);
      result.retired = native.retired;
    }
    return result;
  }

  RunResult step() {
    if (jit.requires_image()) {
      return block_result(jit.run_prepared(1, context.cpu.registers, context));
    }
    RunResult result{.state = context.cpu.execution, .pc = context.cpu.registers.pc};
    const auto capture = detail::fetch(jit.memory(), context, result.pc);
    if (capture.size != 0) { result.opcode = capture.bytes[0]; }
    if (context.fault != Fault::none) {
      result = failed(result);
    } else if (!capture.supported) {
      result.fault = Fault::unsupported_instruction;
    } else {
      result = execute(std::span(capture.bytes).first(capture.size));
    }
    if (result.fault != Fault::none) { context.cpu.failure = result; }
    return result;
  }

  [[gnu::always_inline]] RunResult segment(const std::uint32_t budget, bool &suffix) {
    const auto block =
        suffix ? std::nullopt : jit.run_block(budget, context.cpu.registers, context);
    if (block && block->retired == 0 && context.fault == Fault::none && !jit.requires_image()) {
      suffix = true;
    }
    return block && !suffix ? block_result(*block) : step();
  }

  template <bool Timed>
  [[gnu::always_inline]] RunResult run_native(const std::uint32_t instruction_budget) {
    RunResult result;
    bool suffix = false;
    for (std::uint32_t retired = 0; retired < instruction_budget;) {
      constexpr unsigned interrupt_masks = 0x50;
      const auto masks =
          Timed ? static_cast<Byte>(context.cpu.registers.ccr & interrupt_masks) : Byte{};
      const auto delay = Timed ? context.cpu.irq_delay : Byte{};
      result = segment(instruction_budget - retired, suffix);
      const auto count = result.retired;
      if constexpr (Timed) { observe_interrupt_boundary(masks, delay); }
      retired += result.retired;
      result.retired = retired;
      if constexpr (Timed) {
        if (context.progress.estimated_cycles >= context.cycle_deadline) { break; }
      }
      if (result.fault != Fault::none || result.state != ExecutionState::running ||
          context.yield_requested || count == 0) {
        break;
      }
    }
    return result;
  }
};

Core::Core(const Memory memory, Io *const io)
    : impl_(std::make_unique<Impl>(memory, io != nullptr ? *io : Io{})) {}

Core::~Core() = default;

SemanticImage Core::lower(const CodeImage image, const CompileOptions options) {
  if (!native_supported()) { throw std::runtime_error("Unsupported CPU12 native image host"); }
  return SemanticImage(std::make_shared<const SemanticImage::Impl>(image, options));
}

NativeImage Core::bind(const SemanticImage &image, const NativeSymbols &symbols) {
  if (!image.impl_ || !symbols.owner || symbols.identity != image.identity() ||
      symbols.symbols.size() != image.exports().size()) {
    throw std::invalid_argument(
        "CPU12 native binding has an empty owner or incompatible image identity");
  }
  std::map<std::string_view, const void *> entries;
  for (const auto entry : symbols.symbols) {
    if (entry.name.empty() || entry.name.find('\0') != std::string_view::npos ||
        entry.address == nullptr || !entries.emplace(entry.name, entry.address).second) {
      throw std::invalid_argument("CPU12 native binding has an invalid or duplicate export");
    }
  }
  for (const auto entry : image.exports()) {
    if (!entries.contains(entry.name)) {
      throw std::invalid_argument("CPU12 native binding export is missing: " +
                                  std::string(entry.name));
    }
  }
  const auto program =
      std::make_shared<const detail::llvm19::ImageProgram>(*image.impl_->program, symbols);
  return NativeImage(std::make_shared<const NativeImage::Impl>(image.impl_->interrupts, program));
}

NativeImage Core::compile(const CodeImage image, const CompileOptions options) {
  const auto semantic = lower(image, options);
  const auto native = std::make_shared<const detail::llvm19::NativeModule>(
      semantic.bitcode(), semantic.exports(), options.bindings, semantic.impl_->callbacks);
  return bind(semantic,
              {.owner = native,
               .symbols = native->symbols(),
               .identity = semantic.identity(),
               .native_bytes = native->native_bytes()});
}

void Core::install(const NativeImage &image, const std::uint64_t variant) {
  const std::array variants{variant};
  install(image, variants);
}

void Core::install(const NativeImage &image, const std::span<const std::uint64_t> variants) {
  if (impl_->active) { throw std::logic_error("Cannot install an active CPU12 image"); }
  if (!image.impl_) { throw std::invalid_argument("Cannot install an empty CPU12 image"); }
  impl_->jit.install(image.impl_->program, variants, image.impl_->interrupts);
  impl_->admit_prefix_memory();
  impl_->context.memory_bindings = impl_->jit.bindings();
  impl_->context.memory_grants = impl_->jit.memory().regions();
}

void Core::isolate_reads(const std::span<const LocalRead> reads) {
  if (impl_->active) { throw std::logic_error("Cannot isolate active CPU12 reads"); }
  impl_->jit.isolate_reads(reads);
}

Registers Core::registers() const noexcept {
  return impl_->context.cpu.registers;
}

const State &Core::state() const noexcept {
  return impl_->context.cpu;
}

Progress Core::progress() const noexcept {
  return impl_->context.progress;
}

std::uint64_t Core::foreground_prefix() const {
  const auto &state = impl_->context.cpu;
  if (!impl_->prefix_ready() || state.execution != ExecutionState::running ||
      state.returned_frame) {
    return 0;
  }
  return impl_->jit.foreground_prefix(state);
}

std::uint64_t Core::event_prefix() const {
  const auto &state = impl_->context.cpu;
  if (!impl_->prefix_ready() ||
      (state.execution != ExecutionState::running && state.execution != ExecutionState::waiting)) {
    return 0;
  }
  return impl_->jit.event_prefix(state);
}

std::uint64_t Core::channel_prefix(const PrefixScope scope) const {
  const auto &state = impl_->context.cpu;
  if (!impl_->prefix_ready() ||
      (state.execution != ExecutionState::running && state.execution != ExecutionState::waiting)) {
    return 0;
  }
  if (scope == PrefixScope::events) { return impl_->jit.event_prefix(state, true); }
  if (scope != PrefixScope::foreground || state.execution != ExecutionState::running ||
      state.returned_frame) {
    return 0;
  }
  return impl_->jit.foreground_prefix(state, true);
}

void Core::request_yield() noexcept {
  if (impl_->active) { impl_->context.yield_requested = true; }
}

void Core::set_registers(const Registers registers) {
  if (impl_->active) { throw std::logic_error("Cannot replace registers during CPU12 execution"); }
  if (impl_->jit.transition_pending()) {
    throw std::logic_error("Cannot replace registers during a pending CPU12 transition");
  }
  impl_->context.cpu.registers = registers;
  impl_->context.cpu.failure = {};
  impl_->context.fault = Fault::none;
  impl_->context.cpu.irq_delay = 0;
  impl_->context.cpu.returned_frame = false;
  impl_->context.cpu.execution = ExecutionState::running;
}

void Core::set_memory(const Memory memory) {
  if (impl_->active) { throw std::logic_error("Cannot remap active CPU12 memory"); }
  impl_->jit.set_memory(memory);
  impl_->prefix_admitted = false;
  impl_->context.memory_bindings = impl_->jit.bindings();
  impl_->context.memory_grants = impl_->jit.memory().regions();
}

PreparationReport Core::prepare(const std::span<const Address> established_entries) {
  PreparationReport report;
  if (impl_->active) {
    report.failure = {.fault = Fault::reentrant,
                      .state = impl_->context.cpu.execution,
                      .pc = impl_->context.cpu.registers.pc};
    return report;
  }
  if (!impl_->supported) {
    report.failure = {.fault = Fault::unsupported_host,
                      .state = impl_->context.cpu.execution,
                      .pc = impl_->context.cpu.registers.pc};
    return report;
  }
  impl_->active = true;
  try {
    report = impl_->jit.prepare(established_entries);
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  report.failure.state = impl_->context.cpu.execution;
  return report;
}

RunResult Core::run_instruction(const std::span<const std::uint8_t> bytes) {
  RunResult result{.state = impl_->context.cpu.execution,
                   .pc = impl_->context.cpu.registers.pc,
                   .opcode = bytes.empty() ? std::uint8_t{0} : bytes[0]};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->context.cpu.failure.fault != Fault::none) { return impl_->context.cpu.failure; }
  if (impl_->jit.transition_pending()) { return result; }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  if (impl_->context.cpu.execution != ExecutionState::running) { return result; }
  if (bytes.empty() || bytes.size() > maximum_capture_bytes) {
    result.fault = Fault::invalid_input;
    return result;
  }
  if (impl_->jit.requires_image()) {
    const auto pc = impl_->context.cpu.registers.pc;
    if (impl_->jit.validate_instruction(pc, bytes) == Fault::invalid_input) {
      result.fault = Fault::invalid_input;
      return result;
    }
    return step();
  }
  impl_->active = true;
  try {
    result = impl_->execute(bytes);
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  return result;
}

RunResult Core::step() {
  RunResult result{.state = impl_->context.cpu.execution, .pc = impl_->context.cpu.registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->context.cpu.failure.fault != Fault::none) { return impl_->context.cpu.failure; }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  if (result.state != ExecutionState::running) { return result; }
  impl_->active = true;
  try {
    result = impl_->step();
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  return result;
}

[[gnu::always_inline]] RunResult Core::run(const std::uint32_t instruction_budget) {
  RunResult result{.state = impl_->context.cpu.execution, .pc = impl_->context.cpu.registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->context.cpu.failure.fault != Fault::none) { return impl_->context.cpu.failure; }
  if (instruction_budget == 0) { return result; }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  if (result.state != ExecutionState::running) { return result; }
  impl_->context.yield_requested = false;
  impl_->active = true;
  try {
    result = impl_->deadline_active ? impl_->run_native<true>(instruction_budget)
                                    : impl_->run_native<false>(instruction_budget);
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  return result;
}

RunResult Core::run_until(const std::uint64_t base_cycle_deadline) {
  if (impl_->active) { throw std::logic_error("Recursive CPU12 execution is not permitted"); }
  if (impl_->context.cpu.failure.fault != Fault::none) { return impl_->context.cpu.failure; }
  const DeadlineScope deadline(impl_->deadline_active, impl_->context, base_cycle_deadline);
  constexpr auto bound = detail::maximum_instruction_cycles;
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (base_cycle_deadline > maximum - (bound - 1)) {
    throw std::overflow_error("CPU12 deadline leaves no representable instruction boundary");
  }
  RunResult result{.fault = impl_->context.cpu.failure.fault,
                   .state = impl_->context.cpu.execution,
                   .pc = impl_->context.cpu.registers.pc};
  while (impl_->context.progress.estimated_cycles < base_cycle_deadline &&
         result.fault == Fault::none && result.state == ExecutionState::running &&
         result.retired != std::numeric_limits<std::uint32_t>::max()) {
    const auto available = std::numeric_limits<std::uint32_t>::max() - result.retired;
    const auto retired = result.retired;
    result = run(available);
    const bool stopped = result.retired == 0 || impl_->context.yield_requested;
    result.retired += retired;
    if (stopped) { break; }
  }
  return result;
}

EventResult Core::reset(const std::uint16_t vector) noexcept {
  EventResult result{.pc = impl_->context.cpu.registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  impl_->jit.cancel_transition();
  impl_->context.cpu.failure = {};
  impl_->context.fault = Fault::none;
  impl_->context.cpu.irq_delay = 0;
  impl_->context.cpu.returned_frame = false;
  impl_->active = true;
  const auto target = reset_vector(impl_->jit.memory(), impl_->context, vector);
  if (target == detail::failed_word_read) {
    result = impl_->failed_event(result);
  } else {
    constexpr unsigned masks = 0xd0;
    impl_->context.cpu.registers.pc = static_cast<Address>(target);
    impl_->context.cpu.registers.ccr = static_cast<Byte>(impl_->context.cpu.registers.ccr | masks);
    impl_->context.cpu.execution = ExecutionState::running;
    impl_->context.progress.estimated_cycles += 5;
    result.accepted = true;
  }
  impl_->active = false;
  return result;
}

bool Core::interrupt_ready(const InterruptKind kind) const noexcept {
  if (impl_->active || !impl_->supported || impl_->context.cpu.failure.fault != Fault::none ||
      impl_->jit.transition_pending()) {
    return false;
  }
  constexpr unsigned irq = 0x10;
  constexpr unsigned xirq = 0x40;
  switch (kind) {
    case InterruptKind::maskable:
      return (impl_->context.cpu.registers.ccr & irq) == 0 && impl_->context.cpu.irq_delay == 0;
    case InterruptKind::xirq: return (impl_->context.cpu.registers.ccr & xirq) == 0;
    default: return false;
  }
}

bool Core::interrupt_frame_available() const noexcept {
  if (impl_->active || impl_->context.cpu.failure.fault != Fault::none ||
      impl_->jit.transition_pending()) {
    return false;
  }
  return impl_->context.cpu.execution != ExecutionState::running ||
         impl_->context.cpu.returned_frame;
}

void Core::close_return_window() {
  if (impl_->active || impl_->context.cpu.failure.fault != Fault::none ||
      impl_->jit.transition_pending()) {
    throw std::logic_error("Cannot close an active or failed CPU12 return boundary");
  }
  impl_->context.cpu.returned_frame = false;
}

EventResult Core::interrupt(const InterruptRequest request) {
  EventResult result{.pc = impl_->context.cpu.registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->context.cpu.failure.fault != Fault::none) {
    result.fault = impl_->context.cpu.failure.fault;
    result.address = impl_->context.cpu.failure.address;
    result.value = impl_->context.cpu.failure.value;
    result.access_size = impl_->context.cpu.failure.access_size;
    return result;
  }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  if (request.kind != InterruptKind::maskable && request.kind != InterruptKind::xirq) {
    result.fault = Fault::invalid_input;
    return result;
  }
  if (!interrupt_ready(request.kind)) { return result; }
  const bool reuse = interrupt_frame_available();
  impl_->active = true;
  try {
    impl_->jit.interrupt(request, impl_->context.cpu.registers, impl_->context);
    if (impl_->context.fault != Fault::none) {
      result = impl_->failed_event(result);
    } else {
      result.accepted = true;
      impl_->context.progress.estimated_cycles += reuse ? 6U : 9U;
    }
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  return result;
}

EventResult Core::resume_from_stop() {
  EventResult result{.pc = impl_->context.cpu.registers.pc};
  if (impl_->jit.transition_pending()) { return result; }
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->context.cpu.failure.fault != Fault::none) {
    result.fault = impl_->context.cpu.failure.fault;
    result.address = impl_->context.cpu.failure.address;
    result.value = impl_->context.cpu.failure.value;
    result.access_size = impl_->context.cpu.failure.access_size;
    return result;
  }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  constexpr unsigned x_mask = 0x40;
  if (impl_->context.cpu.execution != ExecutionState::stopped ||
      (impl_->context.cpu.registers.ccr & x_mask) == 0) {
    return result;
  }
  impl_->active = true;
  try {
    impl_->jit.resume_stop(impl_->context.cpu.registers, impl_->context);
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  if (impl_->context.fault != Fault::none) { return impl_->failed_event(result); }
  result.accepted = true;
  return result;
}

Statistics Core::statistics() const noexcept {
  return impl_->jit.statistics();
}

}
