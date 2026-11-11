// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace cpu12::test::public_io {
struct Access {
  Address address = 0, value = 0;
  Byte width = 0, mask = std::numeric_limits<Byte>::max();
  bool write = false;
  State state;
  Progress progress;
  bool operator==(const Access &) const = default;
};

struct Bus {
  std::array<Byte, address_bytes> memory{};
  std::array<Access, 16> trace{};
  const State *state = nullptr;
  const Progress *progress = nullptr;
  unsigned calls = 0, fail_at = std::numeric_limits<unsigned>::max();
  void *observer = nullptr;
  void (*notify)(void *) noexcept = nullptr;
};
}

extern "C" {
bool fixture_read(void *owner, cpu12::Address address, cpu12::Byte &value) noexcept;
bool fixture_write(void *owner, cpu12::ByteWrite access) noexcept;
bool fixture_read_word(void *owner, cpu12::Address address, cpu12::Address &value) noexcept;
bool fixture_write_word(void *owner, cpu12::WordWrite access) noexcept;
bool fixture_read_masked(void *owner, cpu12::MaskedByteRead access, cpu12::Byte &value) noexcept;
}
