// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.hpp"

#include "cpu12/core.hpp"

#include <cstdint>

namespace cpu12::detail {
std::uint32_t read_byte(NativeContext *const context, const std::uint16_t address) noexcept {
  std::uint8_t value = 0;
  if (context->io.read != nullptr) {
    ++context->read_callbacks;
    if (context->io.read(context->io.owner, address, value)) { return value; }
  }
  context->fault = Fault::memory_read;
  context->address = address;
  context->value = value;
  return failed_read;
}

std::uint32_t write_byte(NativeContext *const context,
                         const std::uint16_t address,
                         const std::uint8_t value) noexcept {
  if (context->io.write != nullptr) {
    ++context->write_callbacks;
    if (context->io.write(context->io.owner, {.address = address, .value = value})) { return 1; }
  }
  context->fault = Fault::memory_write;
  context->address = address;
  context->value = value;
  return 0;
}
}
