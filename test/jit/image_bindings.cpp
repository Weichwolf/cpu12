// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/io_abi.hpp"
#include "runtime.hpp"

#include <cstdint>

extern "C" std::uint32_t cpu12_read_byte(cpu12::detail::NativeContext *context,
                                         const std::uint16_t address) noexcept {
  return static_cast<const std::uint8_t *>(context->io.owner)[address];
}

extern "C" std::uint32_t cpu12_read_word(cpu12::detail::NativeContext *context,
                                         const std::uint16_t address) noexcept {
  constexpr unsigned byte_bits = 8;
  const auto *bytes = static_cast<const std::uint8_t *>(context->io.owner);
  const auto next = static_cast<std::uint16_t>(address + 1);
  return (std::uint32_t{bytes[address]} << byte_bits) | bytes[next];
}

extern "C" std::uint32_t cpu12_write_byte(cpu12::detail::NativeContext *context,
                                          const std::uint16_t address,
                                          const std::uint8_t value) noexcept {
  static_cast<std::uint8_t *>(context->io.owner)[address] = value;
  return 1;
}

extern "C" std::uint32_t cpu12_write_word(cpu12::detail::NativeContext *context,
                                          const std::uint16_t address,
                                          const std::uint16_t value) noexcept {
  constexpr unsigned byte_bits = 8;
  auto *bytes = static_cast<std::uint8_t *>(context->io.owner);
  const auto next = static_cast<std::uint16_t>(address + 1);
  bytes[address] = static_cast<std::uint8_t>(value >> byte_bits);
  bytes[next] = static_cast<std::uint8_t>(value);
  return 1;
}

extern "C" std::uint32_t cpu12_read_masked_byte(cpu12::detail::NativeContext *context,
                                                const std::uint16_t address,
                                                const std::uint8_t mask) noexcept {
  return static_cast<const std::uint8_t *>(context->io.owner)[address] & mask;
}
