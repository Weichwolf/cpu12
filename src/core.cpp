// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"

#include "instruction.hpp"
#include "jit.hpp"

#include <bit>
#include <cstdint>
#include <memory>
#include <span>
#if defined(__x86_64__) && defined(__linux__)
#include <cpuid.h>
#endif

namespace cpu12 {
class Core::Impl {
public:
  const bool supported = Core::native_supported();
  Registers registers;
  detail::Jit jit;
};

Core::Core() : impl_(std::make_unique<Impl>()) {}

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

void Core::set_registers(const Registers registers) noexcept {
  impl_->registers = registers;
}

RunResult Core::execute(const std::span<const std::uint8_t> bytes) {
  RunResult result{.pc = impl_->registers.pc, .opcode = bytes.empty() ? std::uint8_t{0} : bytes[0]};
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
  impl_->jit.execute(*instruction, impl_->registers);
  result.retired = 1;
  return result;
}

Statistics Core::statistics() const noexcept {
  return impl_->jit.statistics();
}

void Core::clear_code_cache() {
  impl_->jit.clear();
}
}
