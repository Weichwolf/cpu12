// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fft.hpp"
#include "machine.hpp"
#include "measurement.hpp"
#include "program.hpp"
#include "reference.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace cpu12::bench {
class FftRun {
public:
  FftRun() : machine_(fft_program()) {
    constexpr int coefficient_scale = 32768;
    constexpr int maximum_coefficient = 32767;
    constexpr unsigned input_mask = 4095;
    constexpr unsigned real_step = 97;
    constexpr unsigned imag_step = 53;
    constexpr int input_center = 2048;
    constexpr unsigned real_bias = 13;
    constexpr unsigned imag_bias = 7;
    for (unsigned i = 0; i < fft_points; ++i) {
      unsigned reversed = 0;
      for (unsigned bit = 0; bit < byte_bits; ++bit) {
        reversed = (reversed * 2) + ((i >> bit) & 1U);
      }
      input_[std::size_t{reversed} * 2] = static_cast<std::int16_t>(
          static_cast<int>(((i * real_step) + real_bias) & input_mask) - input_center);
      input_[(reversed * 2) + 1] = static_cast<std::int16_t>(
          static_cast<int>(((i * imag_step) + imag_bias) & input_mask) - input_center);
    }
    for (unsigned i = 0; i < fft_points / 2; ++i) {
      const auto angle = -2.0 * std::numbers::pi * i / fft_points;
      coefficients_[std::size_t{i} * 2] =
          static_cast<std::int16_t>(std::clamp(std::lround(std::cos(angle) * coefficient_scale),
                                               -long{maximum_coefficient},
                                               long{maximum_coefficient}));
      coefficients_[(i * 2) + 1] =
          static_cast<std::int16_t>(std::clamp(std::lround(std::sin(angle) * coefficient_scale),
                                               -long{maximum_coefficient},
                                               long{maximum_coefficient}));
      const auto at = static_cast<Address>(fft_twiddles + (i * twiddle_bytes));
      put(machine_.ram, at, static_cast<unsigned>(coefficients_[std::size_t{i} * 2]));
      put(machine_.ram, at + 2, static_cast<unsigned>(coefficients_[(i * 2) + 1]));
      put(machine_.ram, at + 4, static_cast<unsigned>(-coefficients_[(i * 2) + 1]));
    }
    expected_ = input_;
    fft(expected_, coefficients_);
  }

  Measurement execute(Counters &counters) {
    machine_.core.restore(machine_.initial);
    Measurement total;
    for (unsigned pass = 0; pass < fft_repetitions; ++pass) {
      for (unsigned i = 0; i < input_.size(); ++i) {
        put(machine_.ram,
            static_cast<Address>(fft_data + (i * 2)),
            static_cast<unsigned>(input_[i]));
      }
      machine_.core.set_registers({.pc = code_base, .sp = stack_top});
      counters.start();
      const auto result = machine_.core.run();
      auto measured = counters.stop();
      measured.retired = result.retired;
      total += measured;
      require(result.fault == Fault::none &&
                  machine_.core.state().execution == ExecutionState::waiting &&
                  machine_.core.registers().pc == machine_.program.at("end"),
              "FFT native execution did not finish");
      for (unsigned i = 0; i < expected_.size(); ++i) {
        require(static_cast<std::uint16_t>(
                    get(machine_.ram, static_cast<Address>(fft_data + (i * 2)))) ==
                    std::bit_cast<std::uint16_t>(expected_[i]),
                "FFT complete complex output differs from the portable reference");
      }
    }
    constexpr std::uint64_t instructions_per_transform = 98891;
    require(total.retired == instructions_per_transform * fft_repetitions,
            "FFT independent instruction count mismatch");
    return total;
  }

  Measurement portable(Counters &counters) const {
    Measurement total;
    for (unsigned pass = 0; pass < fft_repetitions; ++pass) {
      auto data = input_;
      counters.start();
      fft(data, coefficients_);
      total += counters.stop();
      require(data == expected_, "Portable FFT comparison changed its complete result");
    }
    return total;
  }

  [[nodiscard]] std::uint64_t native_bytes() const { return machine_.image.report().native_bytes; }

  [[nodiscard]] double preparation_seconds() const { return machine_.preparation; }

private:
  Machine machine_;
  std::array<std::int16_t, std::size_t{fft_points} * 2> input_{}, expected_{};
  std::array<std::int16_t, fft_points> coefficients_{};
};
}
