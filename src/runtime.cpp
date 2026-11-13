// SPDX-License-Identifier: GPL-3.0-only
#include "runtime.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/llvm/io_abi.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

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

extern "C" std::uint32_t cpu12_read_byte(cpu12::detail::NativeContext *context,
                                         const std::uint16_t address) noexcept {
  return cpu12::detail::read_byte(context, address);
}

extern "C" std::uint32_t cpu12_read_word(cpu12::detail::NativeContext *context,
                                         const std::uint16_t address) noexcept {
  return cpu12::detail::read_word(context, address);
}

extern "C" std::uint32_t cpu12_write_byte(cpu12::detail::NativeContext *context,
                                          const std::uint16_t address,
                                          const std::uint8_t value) noexcept {
  return cpu12::detail::write_byte(context, address, value);
}

extern "C" std::uint32_t cpu12_write_word(cpu12::detail::NativeContext *context,
                                          const std::uint16_t address,
                                          const std::uint16_t value) noexcept {
  return cpu12::detail::write_word(context, address, value);
}

extern "C" std::uint32_t cpu12_read_masked_byte(cpu12::detail::NativeContext *context,
                                                const std::uint16_t address,
                                                const std::uint8_t mask) noexcept {
  return cpu12::detail::read_masked_byte(context, {.address = address, .mask = mask});
}

namespace {
struct Window {
  const cpu12::Ram *region;
  std::size_t high, low;
};

template <unsigned Bytes>
std::optional<Window> window(const std::span<const cpu12::Ram> regions,
                             const cpu12::Address address,
                             const bool write) noexcept {
  for (const auto &region : regions) {
    if (address < region.first || (write && !region.writable)) { continue; }
    const auto high = static_cast<std::size_t>(address - region.first);
    const auto next = static_cast<cpu12::Address>(address + 1U);
    const auto low = Bytes == 2 ? std::size_t{static_cast<unsigned>(next - region.first)} : high;
    if (high < region.bytes.size() && low < region.bytes.size()) {
      return Window{.region = &region, .high = high, .low = low};
    }
  }
  return std::nullopt;
}

template <unsigned Bytes, bool Write = false>
std::optional<std::uint32_t> mapped(cpu12::detail::NativeContext *const context,
                                    const cpu12::WordWrite access) noexcept {
  const auto selected = window<Bytes>(context->memory_grants, access.address, Write);
  if (!selected) { return std::nullopt; }
  const auto [storage, high, low] = *selected;
  const auto &region = *storage;
  if constexpr (Write) {
    region.bytes[high] = static_cast<cpu12::Byte>(Bytes == 2 ? access.value >> 8U : access.value);
    if constexpr (Bytes == 2) { region.bytes[low] = static_cast<cpu12::Byte>(access.value); }
    if (!region.known.empty()) { region.known[high] = region.known[low] = 1; }
    return 1;
  } else {
    if (!region.known.empty() && (region.known[high] == 0 || region.known[low] == 0)) {
      return std::nullopt;
    }
    const std::uint32_t first = region.bytes[high];
    return Bytes == 2 ? (first << 8U) | region.bytes[low] : first;
  }
}
}

extern "C" std::uint32_t cpu12_mapped_read_byte(cpu12::detail::NativeContext *context,
                                                const std::uint16_t address) noexcept {
  if (const auto value = mapped<1>(context, {.address = address, .value = 0})) { return *value; }
  return cpu12_read_byte(context, address);
}

extern "C" std::uint32_t cpu12_mapped_read_word(cpu12::detail::NativeContext *context,
                                                const std::uint16_t address) noexcept {
  if (const auto value = mapped<2>(context, {.address = address, .value = 0})) { return *value; }
  return cpu12_read_word(context, address);
}

extern "C" std::uint32_t cpu12_mapped_write_byte(cpu12::detail::NativeContext *context,
                                                 const std::uint16_t address,
                                                 const std::uint8_t value) noexcept {
  if (const auto accepted = mapped<1, true>(context, {.address = address, .value = value})) {
    return *accepted;
  }
  return cpu12_write_byte(context, address, value);
}

extern "C" std::uint32_t cpu12_mapped_write_word(cpu12::detail::NativeContext *context,
                                                 const std::uint16_t address,
                                                 const std::uint16_t value) noexcept {
  if (const auto accepted = mapped<2, true>(context, {.address = address, .value = value})) {
    return *accepted;
  }
  return cpu12_write_word(context, address, value);
}

extern "C" std::uint32_t cpu12_mapped_read_masked_byte(cpu12::detail::NativeContext *context,
                                                       const std::uint16_t address,
                                                       const std::uint8_t mask) noexcept {
  if (const auto value = mapped<1>(context, {.address = address, .value = 0})) {
    return *value & mask;
  }
  return cpu12_read_masked_byte(context, address, mask);
}
