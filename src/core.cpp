// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"

#include "instruction.hpp"
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
class Core::Impl {
public:
  Impl(const Ram ram, const Io io) : context{.io = io}, jit(ram) {
    constexpr std::size_t address_bytes = 65536;
    if (ram.bytes.size() > address_bytes - ram.first) {
      throw std::invalid_argument("RAM window exceeds the 16-bit CPU12 address space");
    }
  }

  const bool supported = Core::native_supported();
  Registers registers;
  detail::NativeContext context;
  detail::Jit jit;
  RunResult failure;
  bool active = false;
};

Core::Core(const Ram ram, const Io io) : impl_(std::make_unique<Impl>(ram, io)) {}

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

RunResult Core::execute(const std::span<const std::uint8_t> bytes) {
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
  if (bytes.empty() || bytes.size() > 3) {
    result.fault = Fault::invalid_input;
    return result;
  }
  const auto instruction = detail::decode(bytes);
  if (!instruction) {
    result.fault = Fault::unsupported_instruction;
    return result;
  }
  impl_->active = true;
  try {
    impl_->jit.execute(*instruction, impl_->registers, impl_->context);
  } catch (...) {
    impl_->active = false;
    throw;
  }
  impl_->active = false;
  if (impl_->context.fault == Fault::none) {
    result.retired = 1;
  } else {
    result.fault = impl_->context.fault;
    result.address = impl_->context.address;
    result.value = impl_->context.value;
    result.access_size = impl_->context.access_size;
    impl_->failure = result;
  }
  return result;
}

Statistics Core::statistics() const noexcept {
  auto result = impl_->jit.statistics();
  result.read_callbacks = impl_->context.read_callbacks;
  result.write_callbacks = impl_->context.write_callbacks;
  return result;
}

void Core::clear_code_cache() {
  if (impl_->active) { throw std::logic_error("Cannot invalidate active CPU12 code"); }
  impl_->jit.clear();
}
}
