// SPDX-License-Identifier: GPL-3.0-only
#include "packet_fft.hpp"
#include "packet_report.hpp"

int main(const int argc, const char *const *const argv) {
  using namespace cpu12::test;
  constexpr packet::Scenario scenario{
      .name = "packet-fft-stream",
      .qualification_scope = "fixed-point-fft-stream-only",
      .detail = "points=16 channels=6 sample_rate=48000 input=guest-word-ramp",
      .host_comparison = "input-ramp-and-transform-all-memory-registers",
      .packets_per_second = packet_fft::packets_per_second,
      .instructions = packet_fft::instructions_per_packet};
  return packet::run<packet_fft::Guest, packet_fft::Host>(
      argc, argv, scenario, packet_fft::verify_cases);
}
