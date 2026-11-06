// SPDX-License-Identifier: GPL-3.0-only
#include "packet_checksum.hpp"
#include "packet_report.hpp"

int main(const int argc, const char *const *const argv) {
  using namespace cpu12::test;
  constexpr packet::Scenario scenario{.name = "packet-word-checksum",
                                      .qualification_scope = "checksum-only",
                                      .detail = "words_per_packet=7499",
                                      .host_comparison = "word-recurrence-all-memory-sum-carry",
                                      .packets_per_second = packet_checksum::packets_per_second,
                                      .instructions = (packet_checksum::maximum_words * 4U) + 3};
  return packet::run<packet_checksum::Guest, packet_checksum::Host>(
      argc, argv, scenario, packet_checksum::verify_cases);
}
