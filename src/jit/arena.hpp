// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace cpu12::detail {
// Append-only dual mappings: generated entry addresses remain valid until destruction.
// Writable storage is private; consumers receive only the RX address.
class ExecutableArena {
public:
  ExecutableArena();
  ~ExecutableArena();
  ExecutableArena(const ExecutableArena &) = delete;
  ExecutableArena &operator=(const ExecutableArena &) = delete;
  [[nodiscard]] const void *append(std::span<const std::uint8_t> bytes);
  [[nodiscard]] std::size_t reserved_bytes() const noexcept;

private:
  class Segment;
  std::vector<std::unique_ptr<Segment>> segments_;
};
}
