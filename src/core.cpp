// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"

#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "jit.hpp"
#include "runtime.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#if defined(__x86_64__) && defined(__linux__)
#include <cpuid.h>
#endif

namespace cpu12 {
namespace {
const Byte *known_byte(const Ram ram, const Address address) noexcept {
  if (address < ram.first) { return nullptr; }
  const auto offset = static_cast<std::size_t>(address - ram.first);
  return offset < ram.bytes.size() ? ram.bytes.data() + offset : nullptr;
}

std::uint32_t
reset_vector(const Ram ram, detail::NativeContext &context, const Address vector) noexcept {
  const auto *high = known_byte(ram, vector);
  const auto *low = known_byte(ram, static_cast<Address>(vector + 1U));
  if (high == nullptr || low == nullptr) { return detail::read_word(&context, vector); }
  constexpr unsigned byte_bits = 8;
  return (static_cast<std::uint32_t>(*high) << byte_bits) | *low;
}
}

class Core::Impl {
public:
  Impl(const Ram memory, const Io io, const Options options)
      : ram(memory), context{.io = io}, jit(memory, options) {}

  const bool supported = Core::native_supported();
  Registers registers;
  Ram ram;
  detail::NativeContext context;
  detail::Jit jit;
  RunResult failure;
  bool active = false;

  RunResult failed(RunResult result) noexcept {
    result.state = context.state;
    result.fault = context.fault;
    result.address = context.address;
    result.value = context.value;
    result.access_size = context.access_size;
    if (context.access_size == 1 &&
        (context.fault == Fault::memory_read || context.fault == Fault::instruction_fetch)) {
      result.read_mask = context.read_mask;
    }
    failure = result;
    return result;
  }

  RunResult execute(const std::span<const std::uint8_t> bytes) {
    RunResult result{.pc = registers.pc, .opcode = bytes[0]};
    if (!jit.execute(bytes, registers, context)) {
      result.fault = Fault::unsupported_instruction;
    } else if (context.fault != Fault::none) {
      return failed(result);
    } else {
      result.retired = 1;
      result.state = context.state;
    }
    return result;
  }
};

Core::Core(const Memory memory, Io *const io, const Options options)
    : impl_(std::make_unique<Impl>(memory.storage(), io != nullptr ? *io : Io{}, options)) {}

Core::Core(const Ram ram, const Io io)
    : impl_(std::make_unique<Impl>(Memory::window(ram).storage(), io, Options{})) {}

Core::~Core() = default;

bool Core::native_supported() noexcept {
#if defined(__x86_64__) && defined(__linux__)
  if (std::endian::native != std::endian::little) { return false; }
  constexpr unsigned extended_features = 0x80000001;
  constexpr unsigned lahf_lm = 1;
  unsigned eax = 0;
  unsigned ebx = 0;
  unsigned ecx = 0;
  unsigned edx = 0;
  return __get_cpuid(extended_features, &eax, &ebx, &ecx, &edx) != 0 && (ecx & lahf_lm) != 0;
#else
  return false;
#endif
}

Registers Core::registers() const noexcept {
  return impl_->registers;
}

ExecutionState Core::state() const noexcept {
  return impl_->context.state;
}

Checkpoint Core::checkpoint() const {
  if (impl_->active) { throw std::logic_error("Cannot checkpoint active CPU12 execution"); }
  return {.registers = impl_->registers,
          .failure = impl_->failure,
          .state = impl_->context.state,
          .irq_delay =
              static_cast<std::uint8_t>(impl_->context.boundary_control & detail::irq_delay_mask),
          .returned_frame =
              (impl_->context.boundary_control & detail::returned_frame_boundary) != 0};
}

void Core::restore(const Checkpoint &checkpoint) {
  if (impl_->active) { throw std::logic_error("Cannot restore active CPU12 execution"); }
  const bool idle = checkpoint.state != ExecutionState::running;
  constexpr unsigned stop_disable = 0x80;
  if (checkpoint.state > ExecutionState::stopped || checkpoint.irq_delay > 1 ||
      (idle && (checkpoint.irq_delay != 0 || checkpoint.returned_frame)) ||
      (checkpoint.returned_frame && checkpoint.irq_delay != 0) ||
      (checkpoint.state == ExecutionState::stopped &&
       (checkpoint.registers.ccr & stop_disable) != 0) ||
      checkpoint.failure.fault > Fault::instruction_fetch ||
      (checkpoint.failure.fault != Fault::none &&
       (checkpoint.failure.retired != 0 || checkpoint.failure.pc != checkpoint.registers.pc ||
        checkpoint.failure.state != checkpoint.state))) {
    throw std::invalid_argument("Inconsistent CPU12 checkpoint boundary");
  }
  impl_->registers = checkpoint.registers;
  impl_->failure = checkpoint.failure;
  impl_->context.fault = checkpoint.failure.fault;
  impl_->context.address = checkpoint.failure.address;
  impl_->context.value = checkpoint.failure.value;
  impl_->context.access_size = checkpoint.failure.access_size;
  impl_->context.read_mask = checkpoint.failure.read_mask;
  impl_->context.boundary_control =
      checkpoint.returned_frame ? detail::returned_frame_boundary : checkpoint.irq_delay;
  impl_->context.state = checkpoint.state;
}

void Core::set_registers(const Registers registers) {
  if (impl_->active) { throw std::logic_error("Cannot replace registers during CPU12 execution"); }
  impl_->registers = registers;
  impl_->failure = {};
  impl_->context.fault = Fault::none;
  impl_->context.boundary_control = 0;
  impl_->context.state = ExecutionState::running;
}

RunResult Core::run_instruction(const std::span<const std::uint8_t> bytes) {
  RunResult result{.state = state(),
                   .pc = impl_->registers.pc,
                   .opcode = bytes.empty() ? std::uint8_t{0} : bytes[0]};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->failure.fault != Fault::none) { return impl_->failure; }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  if (state() != ExecutionState::running) { return result; }
  if (bytes.empty() || bytes.size() > maximum_capture_bytes) {
    result.fault = Fault::invalid_input;
    return result;
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
  RunResult result{.state = state(), .pc = impl_->registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->failure.fault != Fault::none) { return impl_->failure; }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  if (state() != ExecutionState::running) { return result; }
  impl_->active = true;
  try {
    const auto capture = detail::fetch(impl_->ram, impl_->context, result.pc);
    if (capture.size != 0) { result.opcode = capture.bytes[0]; }
    if (impl_->context.fault != Fault::none) {
      result = impl_->failed(result);
    } else if (!capture.supported) {
      result.fault = Fault::unsupported_instruction;
    } else {
      result = impl_->execute(std::span(capture.bytes).first(capture.size));
    }
    if (result.fault != Fault::none) { impl_->failure = result; }
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  return result;
}

RunResult Core::run(const std::uint32_t instruction_budget) {
  RunResult result{.state = state(), .pc = impl_->registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->failure.fault != Fault::none) { return impl_->failure; }
  for (std::uint32_t retired = 0; retired < instruction_budget;) {
    result = step();
    retired += result.retired;
    result.retired = retired;
    if (result.fault != Fault::none || result.state != ExecutionState::running) { break; }
  }
  return result;
}

EventResult Core::reset(const std::uint16_t vector) noexcept {
  EventResult result{.pc = impl_->registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  impl_->failure = {};
  impl_->context.fault = Fault::none;
  impl_->context.boundary_control = 0;
  impl_->active = true;
  const auto target = reset_vector(impl_->ram, impl_->context, vector);
  if (target == detail::failed_word_read) {
    const auto failed = impl_->failed({.pc = result.pc});
    result.fault = failed.fault;
    result.address = failed.address;
    result.value = failed.value;
    result.access_size = failed.access_size;
  } else {
    constexpr unsigned masks = 0xd0;
    impl_->registers.pc = static_cast<Address>(target);
    impl_->registers.ccr = static_cast<Byte>(impl_->registers.ccr | masks);
    impl_->context.state = ExecutionState::running;
    result.accepted = true;
  }
  impl_->active = false;
  return result;
}

bool Core::interrupt_ready(const InterruptKind kind) const noexcept {
  if (impl_->active || !impl_->supported || impl_->failure.fault != Fault::none) { return false; }
  constexpr unsigned irq = 0x10;
  constexpr unsigned xirq = 0x40;
  switch (kind) {
    case InterruptKind::maskable:
      return (impl_->registers.ccr & irq) == 0 &&
             (impl_->context.boundary_control & detail::irq_delay_mask) == 0;
    case InterruptKind::xirq: return (impl_->registers.ccr & xirq) == 0;
    default: return false;
  }
}

bool Core::interrupt_frame_available() const noexcept {
  if (impl_->active || impl_->failure.fault != Fault::none) { return false; }
  return state() != ExecutionState::running ||
         (impl_->context.boundary_control & detail::returned_frame_boundary) != 0;
}

EventResult Core::interrupt(const InterruptRequest request) {
  EventResult result{.pc = impl_->registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->failure.fault != Fault::none) {
    result.fault = impl_->failure.fault;
    result.address = impl_->failure.address;
    result.value = impl_->failure.value;
    result.access_size = impl_->failure.access_size;
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
  impl_->active = true;
  try {
    impl_->jit.interrupt(request, impl_->registers, impl_->context);
    if (impl_->context.fault != Fault::none) {
      const auto failed = impl_->failed({.pc = result.pc});
      result.fault = failed.fault;
      result.address = failed.address;
      result.value = failed.value;
      result.access_size = failed.access_size;
    } else {
      result.accepted = true;
    }
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  return result;
}

EventResult Core::resume_from_stop() {
  EventResult result{.pc = impl_->registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->failure.fault != Fault::none) {
    result.fault = impl_->failure.fault;
    result.address = impl_->failure.address;
    result.value = impl_->failure.value;
    result.access_size = impl_->failure.access_size;
    return result;
  }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
  constexpr unsigned x_mask = 0x40;
  if (state() != ExecutionState::stopped || (impl_->registers.ccr & x_mask) == 0) { return result; }
  impl_->active = true;
  try {
    impl_->jit.resume_stop(impl_->registers, impl_->context);
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  result.accepted = true;
  return result;
}

Statistics Core::statistics() const noexcept {
  auto result = impl_->jit.statistics();
  result.read_callbacks = impl_->context.read_callbacks;
  result.write_callbacks = impl_->context.write_callbacks;
  result.fetch_callbacks = impl_->context.fetch_callbacks;
  return result;
}

void Core::clear_code_cache() {
  if (impl_->active) { throw std::logic_error("Cannot invalidate active CPU12 code"); }
  impl_->jit.clear();
}
}
