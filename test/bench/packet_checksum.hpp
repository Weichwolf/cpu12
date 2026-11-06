// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "word_checksum_host.hpp"

#include <array>
#include <cstdint>

namespace cpu12::test::packet_checksum {
using Image = std::array<Byte, address_bytes>;
inline constexpr Address entry = 0x0200, input = 0x4000, accumulator = 0x9000, stack = 0xf000;
inline constexpr Address maximum_words = 7499;
inline constexpr unsigned packets_per_second = 1000, word_bytes = 2;
inline constexpr unsigned word_mask = 0xffff, byte_mask = 0xff, byte_bits = 8;
inline constexpr Byte retained_flags = 0xf0;

struct Case {
  Address words = maximum_words, seed = 0;
  Byte ccr = retained_flags;
};

[[nodiscard]] std::uint32_t instructions(Case point);
[[nodiscard]] Image image(Case point);
[[nodiscard]] Registers expected(Case point, std::uint64_t packets);
void verify_memory(const Image &ram, Case point, std::uint64_t packets);

class Guest {
public:
  explicit Guest(Case point);
  void packet();
  void verify() const;

  [[nodiscard]] std::uint64_t retired() const noexcept { return retired_; }

  [[nodiscard]] std::uint64_t compiled() const noexcept;

private:
  Case point_;
  Image ram_;
  std::array<Ram, 5> regions_;
  Core core_;
  std::uint64_t packets_ = 0, retired_ = 0;
};

class Host {
public:
  explicit Host(Case point);
  void packet();
  void verify() const;

private:
  Case point_;
  Image ram_;
  checksum::HostResult result_;
  std::uint64_t packets_ = 0;
};

void verify_cases();
}
