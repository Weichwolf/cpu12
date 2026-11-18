// SPDX-License-Identifier: GPL-3.0-only
#include "control_run.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fft_run.hpp"
#include "filters.hpp"
#include "machine.hpp"
#include "measurement.hpp"
#include "program.hpp"
#include "reference.hpp"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cpu12::bench {
namespace {
std::string hash(const std::span<const std::int16_t> output) {
  llvm::SHA256 digest;
  for (const auto sample : output) {
    const auto value = static_cast<std::uint32_t>(static_cast<std::int32_t>(sample));
    std::array<Byte, 4> word{};
    for (unsigned i = 0; i < word.size(); ++i) {
      word[i] = static_cast<Byte>(value >> (i * byte_bits));
    }
    digest.update(llvm::ArrayRef(word));
  }
  std::ostringstream text;
  for (const auto byte : digest.final()) {
    text << std::hex << std::setfill('0') << std::setw(2) << unsigned{byte};
  }
  return text.str();
}

struct FilterFixture {
  bool biquads;
  static constexpr std::array<std::int16_t, biquad_coefficient_count> biquad_coefficients{
      8192, 4096, 2048, -16384, 4096, 16384, -8192, 4096, 8192,   4096,
      4096, 8192, 4096, -24576, 8192, 16384, 8192,  4096, -16384, 8192};
  static constexpr std::array<std::int16_t, 8> initial_state{
      32767, -32768, -32768, 32767, 24576, -16384, -24576, 16384};
  std::array<std::int16_t, 8> final_state{};
  std::vector<std::int16_t> signal =
      std::vector<std::int16_t>((std::size_t{frames} * blocks) + taps - 1);
  std::array<std::int16_t, taps> coefficients{};
  std::vector<std::int16_t> expected = std::vector<std::int16_t>(std::size_t{frames} * blocks);

  explicit FilterFixture(bool cascade) : biquads(cascade) {
    constexpr std::uint32_t seed = 0x243f6a88;
    constexpr std::uint32_t multiplier = 1664525;
    constexpr std::uint32_t increment = 1013904223;
    constexpr unsigned pulse_period = 97;
    constexpr unsigned coefficient_step = 17;
    constexpr unsigned coefficient_period = 31;
    constexpr std::int32_t scale = 32768;
    constexpr std::int32_t center = 15;
    constexpr std::int32_t coefficient_scale = 32;
    auto random = seed;
    for (unsigned i = 0; i < frames * blocks; ++i) {
      random = static_cast<std::uint32_t>((std::uint64_t{random} * multiplier) + increment);
      auto value = (static_cast<std::int32_t>(random >> 16) - scale) / 2;
      if (i % pulse_period == 0) { value = scale - 1; }
      if (i % pulse_period == 1) { value = -scale; }
      signal[i + taps - 1] = static_cast<std::int16_t>(value);
    }
    for (unsigned i = 0; i < taps; ++i) {
      coefficients[i] = static_cast<std::int16_t>(
          (static_cast<int>((i * coefficient_step) % coefficient_period) - center) *
          coefficient_scale);
    }
    if (biquads) {
      final_state =
          biquad(std::span(signal).subspan(taps - 1), biquad_coefficients, initial_state, expected);
      require(hash(expected) == "b598303fc1e7736b8ee0bc5175afe8baf38d6d9ef81f833f79724ab5a0a785ba",
              "Portable biquad output does not match the common Q1.15 contract hash");
      require(hash(final_state) ==
                  "c013f34a4563d1948b54c15f1d7ebc48337af2792c24968c43f1a98e1e063c35",
              "Portable biquad state does not match the common Q1.15 contract hash");
      std::ranges::copy(biquad_coefficients, coefficients.begin());
      for (unsigned stage = 0; stage < 4; ++stage) {
        coefficients[(stage * 5) + 3] = static_cast<std::int16_t>(-coefficients[(stage * 5) + 3]);
        coefficients[(stage * 5) + 4] = static_cast<std::int16_t>(-coefficients[(stage * 5) + 4]);
      }
      return;
    }
    fir(signal, coefficients, expected);
    require(hash(expected) == "01e342abb432c2b44339220d04d3376c55ec4ae30cb71ca783c7db9d3639c81b",
            "Portable FIR does not match the common Q1.15 contract hash");
  }
};

class FirRun {
public:
  explicit FirRun(const FilterFixture &fixture, const bool events)
      : fixture_(fixture),
        events_(events),
        machine_(fixture.biquads ? biquad_program() : fir_program(), !fixture.biquads) {}

  Measurement execute(Counters &counters) {
    machine_.core.restore(machine_.initial);
    put(machine_.ram, irq_counter, 0);
    put(machine_.ram, irq_counter + 2, 0);
    if (fixture_.biquads) {
      for (unsigned i = 0; i < FilterFixture::initial_state.size(); ++i) {
        put(machine_.ram,
            static_cast<Address>(filter_state + (i * 2)),
            static_cast<unsigned>(FilterFixture::initial_state[i]));
      }
    }
    std::vector<std::int16_t> actual;
    actual.reserve(std::size_t{frames} * blocks);
    for (unsigned i = 0; i < taps; ++i) {
      put(machine_.ram,
          static_cast<Address>(coefficients + (i * 2)),
          static_cast<unsigned>(fixture_.coefficients[i]));
    }
    Measurement total;
    Events scheduled;
    for (unsigned block = 0; block < blocks; ++block) {
      for (unsigned i = 0; i < frames + (fixture_.biquads ? 0U : taps - 1); ++i) {
        put(machine_.ram,
            static_cast<Address>(input + (i * 2)),
            static_cast<unsigned>(
                fixture_.signal[(block * frames) + i + (fixture_.biquads ? taps - 1 : 0U)]));
      }
      machine_.core.set_registers({.pc = code_base, .sp = stack_top});
      counters.start();
      const auto retired = execute_block(events_, scheduled);
      auto sample = counters.stop();
      sample.retired = retired;
      total += sample;
      require(machine_.core.state().execution == ExecutionState::waiting &&
                  machine_.core.registers().pc == machine_.program.at("end"),
              "FIR did not finish its complete block");
      for (unsigned i = 0; i < frames; ++i) {
        actual.push_back(std::bit_cast<std::int16_t>(
            static_cast<std::uint16_t>(get(machine_.ram, static_cast<Address>(output + (i * 2))))));
      }
    }
    require(actual == fixture_.expected, "Native FIR differs from the complete portable output");
    require(hash(actual) == hash(fixture_.expected), "Native FIR hash mismatch");
    if (!events_ && !fixture_.biquads) {
      constexpr std::uint64_t instructions_per_block = 297989;
      constexpr std::uint64_t cycles_per_block = 1392655;
      require(total.retired == blocks * instructions_per_block &&
                  machine_.core.progress().estimated_cycles == blocks * cycles_per_block,
              "FIR instruction/base-cycle accounting differs from its independent count");
    }
    if (events_) {
      constexpr unsigned event_count = 87747;
      constexpr std::uint64_t instructions = 76987160;
      constexpr std::uint64_t cycles = 359415331;
      require(scheduled.interrupts == event_count && total.retired == instructions &&
                  machine_.core.progress().estimated_cycles == cycles,
              "K6 exact event schedule or handler instruction/base-cycle count mismatch");
    }
    if (fixture_.biquads) {
      std::array<std::int16_t, 8> state{};
      for (unsigned i = 0; i < state.size(); ++i) {
        state[i] = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(
            get(machine_.ram, static_cast<Address>(filter_state + (i * 2)))));
      }
      require(state == fixture_.final_state,
              "Native biquad final state differs from the complete reference");
    }
    require(((get(machine_.ram, irq_counter) << 16) | get(machine_.ram, irq_counter + 2)) ==
                scheduled.interrupts,
            "FIR IRQ counter mismatch");
    return total;
  }

  Measurement portable(Counters &counters) const {
    std::vector<std::int16_t> output(fixture_.expected.size());
    std::array<std::int16_t, 8> state{};
    counters.start();
    if (fixture_.biquads) {
      state = biquad(std::span(fixture_.signal).subspan(taps - 1),
                     FilterFixture::biquad_coefficients,
                     FilterFixture::initial_state,
                     output);
    } else {
      fir(fixture_.signal, fixture_.coefficients, output);
    }
    const auto measured = counters.stop();
    require(output == fixture_.expected && (!fixture_.biquads || state == fixture_.final_state),
            "Portable filter comparison changed its complete result");
    return measured;
  }

  [[nodiscard]] std::uint64_t native_bytes() const { return machine_.image.report().native_bytes; }

  [[nodiscard]] double preparation_seconds() const { return machine_.preparation; }

private:
  struct Events {
    std::uint64_t bound = grant_cycles, irq = event_cycles;
    unsigned interrupts = 0;
  };

  std::uint64_t execute_block(const bool events, Events &scheduled) {
    std::uint64_t retired = 0;
    do {
      const auto result = events ? machine_.core.run_until(scheduled.bound) : machine_.core.run();
      if (result.fault != Fault::none) {
        throw std::runtime_error(
            "FIR fault=" + std::to_string(static_cast<unsigned>(result.fault)) +
            " PC=" + std::to_string(result.pc));
      }
      retired += result.retired;
      if (machine_.core.state().execution == ExecutionState::waiting) { break; }
      if (events && machine_.core.progress().estimated_cycles >= scheduled.irq) {
        require(machine_.core.interrupt({.vector = vector}).accepted, "FIR IRQ was not accepted");
        ++scheduled.interrupts;
        scheduled.irq += event_cycles;
      }
      if (machine_.core.progress().estimated_cycles >= scheduled.bound) {
        scheduled.bound += grant_cycles;
      }
    } while (true);
    return retired;
  }

  static constexpr std::uint64_t grant_cycles = 256, event_cycles = 4096;
  const FilterFixture &fixture_;
  bool events_;
  Machine machine_;
};

template <class Run> void report(Run &run, const std::string_view name, const bool check_only) {
  Counters counters(!check_only);
  if (check_only) {
    const auto measured = run.execute(counters);
    std::cout << name << " checked_guest_instructions=" << measured.retired << '\n';
    return;
  }
  static_cast<void>(run.execute(counters));
  static_cast<void>(run.portable(counters));
  std::array<Measurement, 3> native{};
  std::array<double, 3> portable{};
  for (unsigned trial = 0; trial < native.size(); ++trial) {
    native[trial] = run.execute(counters);
    portable[trial] = run.portable(counters).seconds;
  }
  std::ranges::sort(native, {}, &Measurement::seconds);
  std::ranges::sort(portable);
  const auto &measured = native[1];
  const auto retired = static_cast<double>(measured.retired);
  std::cout << name << ',' << measured.retired << ',';
  if (counters.has_hardware_counters()) {
    std::cout << measured.instructions / retired << ',' << measured.cycles / retired;
  } else {
    std::cout << ',';
  }
  std::cout << ',' << retired / measured.seconds << ',' << measured.seconds / portable[1] << ','
            << run.preparation_seconds() << ',' << run.native_bytes() << '\n';
}

void kernel(const std::string_view name, const bool check_only) {
  if (name == "K4" || name == "K5") {
    ControlRun run(name == "K5");
    report(run, name, check_only);
  } else if (name == "K3") {
    FftRun run;
    report(run, name, check_only);
  } else {
    const FilterFixture fixture(name == "K2");
    FirRun run(fixture, name == "K6");
    report(run, name, check_only);
  }
}
}
}

int main(int argc, char **argv) {
  try {
    using namespace cpu12::bench;
    bool check_only = false;
    std::string_view selected;
    for (int argument = 1; argument < argc; ++argument) {
      const std::string_view value(argv[argument]);
      if (value == "--check") {
        check_only = true;
      } else if (value.size() == 2 && value[0] == 'K' && value[1] >= '1' && value[1] <= '6') {
        selected = value;
      } else {
        throw std::invalid_argument("Usage: cpu12-kernels [--check] [K1|K2|K3|K4|K5|K6]");
      }
    }
    if (!check_only) {
      std::cout << "kernel,guest_instructions,host_instructions_per_GI,host_cycles_per_GI,"
                   "guest_instructions_per_s,native_over_cpp,preparation_s,native_bytes\n";
      std::cout << std::setprecision(10);
    }
    for (const std::string_view name : {"K1", "K2", "K3", "K4", "K5", "K6"}) {
      if (selected.empty() || selected == name) { kernel(name, check_only); }
    }
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
