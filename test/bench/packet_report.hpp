// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "report.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sched.h>
#include <stdexcept>
#include <string_view>

namespace cpu12::test::packet {
using Clock = std::chrono::steady_clock;
constexpr unsigned trials = 9;

struct Scenario {
  std::string_view name, qualification_scope, detail, host_comparison;
  unsigned packets_per_second, instructions;
};

constexpr double million = 1e6;

inline double cpu_seconds() {
  const auto ticks = std::clock();
  if (ticks == static_cast<std::clock_t>(-1)) { throw std::runtime_error("CPU clock unavailable"); }
  return static_cast<double>(ticks) / static_cast<double>(CLOCKS_PER_SEC);
}

struct Phase {
  double wall = 0, cpu = 0, worst_packet = 0;
  unsigned missed = 0;
};

template <class Engine> Phase measure(Engine &engine, const Scenario &scenario) {
  Phase result;
  const auto cpu_begin = cpu_seconds();
  const auto begin = Clock::now();
  for (unsigned packet = 0; packet < scenario.packets_per_second; ++packet) {
    const auto packet_begin = Clock::now();
    engine.packet();
    const auto elapsed = std::chrono::duration<double>(Clock::now() - packet_begin).count();
    result.worst_packet = std::max(result.worst_packet, elapsed);
    if (elapsed > 1.0 / scenario.packets_per_second) { ++result.missed; }
  }
  result.wall = std::chrono::duration<double>(Clock::now() - begin).count();
  result.cpu = cpu_seconds() - cpu_begin;
  return result;
}

struct Results {
  Phase cold;
  std::array<double, trials> guest{}, host{}, cpu{};
  double worst_warm_packet = 0;
  unsigned missed_warm = 0;
  std::uint64_t compiled = 0;
  int allowed_cpus = 0, first_cpu = -1, last_cpu = -1;
};

template <class Guest, class Host>
Results exercise(Guest &guest, Host &host, const Scenario &scenario) {
  if (guest.compiled() != 0) { throw std::runtime_error("Cold packet must discover its code"); }
  cpu_set_t affinity{};
  if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
    throw std::runtime_error("Cannot read benchmark CPU affinity");
  }
  Results result;
  result.allowed_cpus = CPU_COUNT(&affinity);
  result.first_cpu = sched_getcpu();
  if (result.first_cpu < 0) { throw std::runtime_error("Cannot read benchmark starting CPU"); }
  result.cold = measure(guest, scenario);
  result.compiled = guest.compiled();
  guest.verify();
  measure(host, scenario);
  host.verify();
  const auto before = guest.retired();
  for (unsigned trial = 0; trial < trials; ++trial) {
    const auto run_guest = [&] {
      const auto phase = measure(guest, scenario);
      result.guest[trial] = phase.wall;
      result.cpu[trial] = phase.cpu;
      result.worst_warm_packet = std::max(result.worst_warm_packet, phase.worst_packet);
      result.missed_warm += phase.missed;
    };
    const auto run_host = [&] { result.host[trial] = measure(host, scenario).wall; };
    if (trial % 2 == 0) {
      run_guest();
      run_host();
    } else {
      run_host();
      run_guest();
    }
    guest.verify();
    host.verify();
    if (guest.compiled() != result.compiled) {
      throw std::runtime_error("Warm continuous execution compiled another entry");
    }
  }
  const auto count = std::uint64_t{scenario.instructions} * scenario.packets_per_second * trials;
  if (guest.retired() - before != count) {
    throw std::runtime_error("Continuous retirement count");
  }
  result.last_cpu = sched_getcpu();
  if (result.last_cpu < 0) { throw std::runtime_error("Cannot read benchmark ending CPU"); }
  return result;
}

inline double median(std::array<double, trials> values) {
  std::ranges::sort(values);
  return values[trials / 2];
}

inline bool report(const Results &result, const Scenario &scenario) {
  const auto packet_seconds = 1.0 / scenario.packets_per_second;
  const auto guest = median(result.guest);
  const auto host = median(result.host);
  const auto retired = std::uint64_t{scenario.instructions} * scenario.packets_per_second;
  const auto ideal_seconds =
      static_cast<double>(retired) / bench::reference_instructions_per_second;
  const auto deadline = result.cold.worst_packet <= packet_seconds &&
                        result.worst_warm_packet <= packet_seconds && result.missed_warm == 0 &&
                        result.cold.missed == 0;
  const auto hardware = guest <= ideal_seconds;
  const auto matched = host >= bench::target_fraction * guest;
  std::cout << std::fixed << std::setprecision(6) << scenario.name
            << " continuous=YES qualification_scope=" << scenario.qualification_scope
            << "\n  allowed_cpus=" << result.allowed_cpus << " cpu_start=" << result.first_cpu
            << " cpu_end=" << result.last_cpu << "\n  " << scenario.detail
            << " packets_per_second=" << scenario.packets_per_second
            << " measured_input_s=1 guest_instructions=" << retired << " trials=" << trials
            << " full_state_memory=PASS warm_compiled_entries=0 cold_compiled_entries="
            << result.compiled << '\n'
            << "  reference_model=ideal-one-instruction-per-cycle reference_hz="
            << bench::reference_instructions_per_second << " reference_load=" << ideal_seconds
            << " wall_s=" << guest << " process_cpu_s=" << median(result.cpu)
            << " MIPS=" << static_cast<double>(retired) / million / guest
            << " hardware_factor=" << ideal_seconds / guest
            << " hardware_status=" << (hardware ? "PASS" : "FAIL") << '\n'
            << "  packet_budget_s=" << packet_seconds
            << " worst_cold_packet_s=" << result.cold.worst_packet
            << " worst_warm_packet_s=" << result.worst_warm_packet
            << " cold_missed=" << result.cold.missed << " warm_missed=" << result.missed_warm
            << " cold_input_second_s=" << result.cold.wall
            << " packet_deadline=" << (deadline ? "PASS" : "FAIL") << '\n'
            << "  host_asm_s=" << host << " jit_to_host_percent=" << 100.0 * host / guest
            << " jit_target_70=" << (matched ? "PASS" : "FAIL")
            << " host_comparison=" << scenario.host_comparison << '\n'
            << "  scope=continuous-guest-and-host; no-register-or-RAM-restoration; "
               "API-and-packet-timers-included; no-overhead-subtraction; "
               "setup-and-full-state-verification-untimed; no-board-cycle-or-energy-claim\n";
  return deadline && hardware && matched;
}

template <class Guest, class Host>
int run(const int argc, const char *const *const argv, const Scenario &scenario, void (*verify)()) {
  try {
    const std::string_view choice = argc == 2 ? argv[1] : "enforce";
    if (argc > 2 || (choice != "--verify" && choice != "enforce" && choice != "report")) {
      throw std::invalid_argument("Use --verify, report or enforce");
    }
    if (choice == "--verify") {
      verify();
      return 0;
    }
    Guest guest({});
    Host host({});
    const auto met = report(exercise(guest, host, scenario), scenario);
    return choice == "enforce" && !met ? 2 : 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
}
