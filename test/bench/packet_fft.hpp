// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "fft_program.hpp"

#include <array>
#include <cstdint>

namespace cpu12::test::packet_fft {
inline constexpr unsigned channels = 6, sample_rate = 48000;
inline constexpr unsigned packets_per_second = sample_rate / fft::points;
inline constexpr unsigned instructions_per_frame =
    (fft::points * 4) + 2 + (fft::butterfly_count * fft::instructions_per_butterfly) + 1;
inline constexpr unsigned instructions_per_packet = channels * instructions_per_frame;
inline constexpr Address seed_address = 0x4300, input_stride = 257;

struct Case {
  Address seed = 0;
  Byte ccr = fft::transform_retained_flags;
};

[[nodiscard]] fft::Image image(Case point);
[[nodiscard]] std::array<Ram, 5> regions(fft::Image &ram);
void verify(const fft::Image &ram, Registers registers, Case point, std::uint64_t frames);

class Guest {
public:
  explicit Guest(Case point);
  void advance(unsigned frames);

  void packet() { advance(channels); }

  void verify() const;

  [[nodiscard]] std::uint64_t retired() const noexcept { return retired_; }

  [[nodiscard]] std::uint64_t compiled() const noexcept;

private:
  Case point_;
  fft::Image ram_;
  std::array<Ram, 5> regions_;
  Core core_;
  std::uint64_t frames_ = 0, retired_ = 0;
};

class Host {
public:
  explicit Host(Case point, bool cpp = false);
  void advance(unsigned frames);

  void packet() { advance(channels); }

  void verify() const;

private:
  Case point_;
  fft::Image ram_;
  Registers registers_;
  std::uint64_t frames_ = 0;
  bool cpp_;
};

class CppHost {
public:
  explicit CppHost(const Case point) : host_(point, true) {}

  void advance(const unsigned frames) { host_.advance(frames); }

  void packet() { host_.packet(); }

  void verify() const { host_.verify(); }

private:
  Host host_;
};

void verify_cases();
}
