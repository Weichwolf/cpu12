// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.hpp"

#include "cpu12/core.hpp"

#include <cstdint>
#include <limits>

namespace cpu12::detail {
std::uint32_t read_byte(NativeContext *const context, const std::uint16_t address) noexcept {
  std::uint8_t value = 0;
  if (context->io.read != nullptr) {
    if (context->io.read(context->io.owner, address, value)) { return value; }
  }
  context->access_size = 1;
  context->read_mask = std::numeric_limits<std::uint8_t>::max();
  context->fault = Fault::memory_read;
  context->address = address;
  context->value = value;
  return failed_read;
}

std::uint32_t read_masked_byte(NativeContext *const context, const MaskedByteRead access) noexcept {
  std::uint8_t value = 0;
  bool accepted = false;
  if (context->io.read_masked != nullptr) {
    accepted = context->io.read_masked(context->io.owner, access, value);
  } else if (context->io.read != nullptr) {
    accepted = context->io.read(context->io.owner, access.address, value);
  }
  const auto selected = static_cast<std::uint8_t>(value & access.mask);
  if (accepted) { return selected; }
  context->fault = Fault::memory_read;
  context->address = access.address;
  context->value = value;
  context->access_size = 1;
  context->read_mask = access.mask;
  return failed_read;
}

std::uint32_t write_byte(NativeContext *const context,
                         const std::uint16_t address,
                         const std::uint8_t value) noexcept {
  if (context->io.write != nullptr) {
    if (context->io.write(context->io.owner, {.address = address, .value = value})) { return 1; }
  }
  context->access_size = 1;
  context->read_mask = std::numeric_limits<std::uint8_t>::max();
  context->fault = Fault::memory_write;
  context->address = address;
  context->value = value;
  return 0;
}

std::uint32_t read_word(NativeContext *const context, const std::uint16_t address) noexcept {
  std::uint16_t value = 0;
  if (context->io.read_word != nullptr) {
    if (context->io.read_word(context->io.owner, address, value)) { return value; }
  }
  context->fault = Fault::memory_read;
  context->address = address;
  context->value = value;
  context->access_size = 2;
  context->read_mask = std::numeric_limits<std::uint8_t>::max();
  return failed_word_read;
}

std::uint32_t write_word(NativeContext *const context,
                         const std::uint16_t address,
                         const std::uint16_t value) noexcept {
  if (context->io.write_word != nullptr) {
    if (context->io.write_word(context->io.owner, {.address = address, .value = value})) {
      return 1;
    }
  }
  context->fault = Fault::memory_write;
  context->address = address;
  context->value = value;
  context->access_size = 2;
  context->read_mask = std::numeric_limits<std::uint8_t>::max();
  return 0;
}

}
