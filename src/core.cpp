// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"

#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "jit.hpp"
#include "runtime.hpp"

#include <bit>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#if defined(__x86_64__) && defined(__linux__)
#include <cpuid.h>
#endif

namespace cpu12 {
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

  RunResult failed(RunResult result) {
    result.fault = context.fault;
    result.address = context.address;
    result.value = context.value;
    result.access_size = context.access_size;
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

void Core::set_registers(const Registers registers) {
  if (impl_->active) { throw std::logic_error("Cannot replace registers during CPU12 execution"); }
  impl_->registers = registers;
  impl_->failure = {};
  impl_->context.fault = Fault::none;
}

RunResult Core::run_instruction(const std::span<const std::uint8_t> bytes) {
  RunResult result{.pc = impl_->registers.pc, .opcode = bytes.empty() ? std::uint8_t{0} : bytes[0]};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->failure.fault != Fault::none) { return impl_->failure; }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
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
  RunResult result{.pc = impl_->registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->failure.fault != Fault::none) { return impl_->failure; }
  if (!impl_->supported) {
    result.fault = Fault::unsupported_host;
    return result;
  }
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
  RunResult result{.pc = impl_->registers.pc};
  if (impl_->active) {
    result.fault = Fault::reentrant;
    return result;
  }
  if (impl_->failure.fault != Fault::none) { return impl_->failure; }
  for (std::uint32_t retired = 0; retired < instruction_budget;) {
    result = step();
    retired += result.retired;
    result.retired = retired;
    if (result.fault != Fault::none) { break; }
  }
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
