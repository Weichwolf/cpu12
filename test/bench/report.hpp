// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace cpu12::test::bench {
// Conservative peak-IPC reference, not a calibrated board cycle model.
inline constexpr double reference_instructions_per_second = 30000000;
inline constexpr double nanoseconds_per_second = 1e9, target_fraction = 0.7;

inline void report(const std::uint64_t instructions,
                   const double cold_seconds,
                   const double warm_seconds,
                   const double host_seconds) {
  if (instructions == 0 || cold_seconds <= 0 || warm_seconds <= 0 || host_seconds <= 0) {
    throw std::runtime_error("Benchmark needs positive work and measured durations");
  }
  const auto device_seconds = static_cast<double>(instructions) / reference_instructions_per_second;
  std::cout << " reference_model=ideal-one-instruction-per-cycle reference_hz="
            << reference_instructions_per_second
            << " hardware_factor=" << device_seconds / warm_seconds
            << " cold_hardware_factor_lower_bound=" << device_seconds / cold_seconds
            << " hardware_status=" << (device_seconds >= warm_seconds ? "PASS" : "FAIL")
            << " jit_to_host_percent=" << 100.0 * host_seconds / warm_seconds << " jit_target_70="
            << (host_seconds >= target_fraction * warm_seconds ? "PASS" : "FAIL")
            << " cold_scope=single-kernel-not-processing-packet"
            << " qualification_scope=reset-run-diagnostic sustained_gate=NOT-ESTABLISHED";
}
}
