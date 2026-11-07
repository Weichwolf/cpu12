// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fft_host.hpp"
#include "fft_oracle.hpp"
#include "fft_program.hpp"
#include "isa.hpp"
#include "report.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <numbers>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {
using namespace cpu12;
using namespace cpu12::test::fft;
using Clock = std::chrono::steady_clock;

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

Image image() {
  Image ram;
  constexpr Byte sentinel = 0xa5;
  ram.fill(sentinel);
  std::ranges::copy(program, ram.begin() + entry);
  // Nearest Q8.8 samples of exp(-2*pi*i*k/16), k=0..7.
  constexpr std::array<int, points> coefficients{
      256, 0, 237, -98, 181, -181, 98, -237, 0, -256, -98, -237, -181, -181, -237, -98};
  for (std::size_t index = 0; index < coefficients.size(); ++index) {
    word(ram, static_cast<Address>(twiddles + (index * 2)), narrow(coefficients[index]));
  }
  return ram;
}

struct Device {
  Image ram = image();
  std::array<Ram, 5> regions{
      Ram{.bytes = std::span(ram).first(entry)},
      Ram{.first = entry,
          .bytes = std::span(ram).subspan(entry, program.size()),
          .writable = false},
      Ram{.first = return_pc, .bytes = std::span(ram).subspan(return_pc, twiddles - return_pc)},
      Ram{.first = twiddles,
          .bytes = std::span(ram).subspan(twiddles, std::size_t{points} * 2),
          .writable = false},
      Ram{.first = twiddles + (points * 2),
          .bytes = std::span(ram).subspan(twiddles + (points * 2))}};
  Core core{Memory::mapped(regions)};
};

auto inputs() {
  std::array<Samples, 12> result{};
  constexpr Address amplitude = 64;
  result[0][0] = amplitude;
  constexpr std::array<int, points> cosine{
      64, 59, 45, 24, 0, -24, -45, -59, -64, -59, -45, -24, 0, 24, 45, 59};
  constexpr std::array<int, points> sine{
      0, 24, 45, 59, 64, 59, 45, 24, 0, -24, -45, -59, -64, -59, -45, -24};
  for (std::size_t index = 0; index < points; ++index) {
    result[1][index * 2] = 7;
    result[1][(index * 2) + 1] = narrow(-3);
    result[2][index * 2] = narrow(cosine[index]);
    result[2][(index * 2) + 1] = narrow(sine[index]);
    constexpr unsigned modulus = 127;
    constexpr unsigned stride = 17;
    constexpr unsigned offset = 63;
    result[3][index * 2] = narrow(static_cast<int>((index * stride) % modulus) - int{offset});
    result[3][(index * 2) + 1] =
        narrow(static_cast<int>((index * stride + offset) % modulus) - int{offset});
  }
  constexpr std::array<Address, 8> seeds{0, 1, 0x7f, 0x80, 0x7fff, 0x8000, 0xffff, 0x1357};
  for (std::size_t seed = 0; seed < seeds.size(); ++seed) {
    for (std::size_t index = 0; index < result[seed + 4].size(); ++index) {
      constexpr unsigned stride = 257;
      result[seed + 4][index] = static_cast<Address>((seeds[seed] + index * stride) & word_mask);
    }
  }
  return result;
}

void check_dft(const Samples &input, const Image &ram) {
  for (std::size_t bin = 0; bin < points; ++bin) {
    long double real_sum = 0;
    long double imaginary_sum = 0;
    for (std::size_t index = 0; index < points; ++index) {
      const auto angle = -2 * std::numbers::pi_v<long double> * static_cast<long double>(bin) *
                         static_cast<long double>(index) / points;
      const auto re = signed_word(input[index * 2]);
      const auto im = signed_word(input[(index * 2) + 1]);
      real_sum += re * std::cos(angle) - im * std::sin(angle);
      imaginary_sum += re * std::sin(angle) + im * std::cos(angle);
    }
    const auto re = signed_word(word(ram, static_cast<Address>(data + (bin * 4))));
    const auto im = signed_word(word(ram, static_cast<Address>(data + (bin * 4) + 2)));
    constexpr long double quantization_bound = 16;
    require(std::abs(re - real_sum) <= quantization_bound &&
                std::abs(im - imaginary_sum) <= quantization_bound,
            "Fixed-point FFT exceeds the low-amplitude direct-DFT error bound");
  }
}

void memory(const Image &actual, const Image &expected) {
  if (actual == expected) { return; }
  const auto mismatch = std::ranges::mismatch(actual, expected);
  std::cerr << "ram=" << (mismatch.in1 - actual.begin()) << " actual=" << unsigned{*mismatch.in1}
            << " expected=" << unsigned{*mismatch.in2} << '\n';
  throw std::runtime_error("FFT RAM or original STOP frame differs from independent expectations");
}

void guest(Device &device, const Samples &input, const Byte ccr, const Registers expected) {
  reset(device.ram, input);
  device.core.set_registers(initial(ccr));
  const auto result = device.core.run(retired);
  const RunResult wanted{
      .retired = retired, .state = ExecutionState::stopped, .pc = stop_pc, .opcode = isa::stop[0]};
  if (result != wanted || device.core.registers() != expected) {
    std::cerr << "retired=" << result.retired << "/" << retired
              << " fault=" << unsigned{static_cast<Byte>(result.fault)} << " pc=" << result.pc
              << "/" << stop_pc << '\n';
    throw std::runtime_error(
        "FFT guest registers or retirement differ from independent expectations");
  }
  require(device.core.state().failure == RunResult{} && !device.core.state().returned_frame &&
              device.core.state().irq_delay == 0,
          "FFT must not hide a fault, return frame or delayed interrupt");
}

void native(Image &ram, const Samples &input, const Byte ccr, const Registers expected) {
  reset(ram, input);
  const auto state = host(ram, ccr);
  require(state == expected, "Host ASM FFT disagrees with independent fixed-point registers");
  frame(ram, state);
}

void validation() {
  Device device;
  const auto cases = inputs();
  constexpr unsigned stop_disabled = 0x80;
  constexpr unsigned dft_cases = 4;
  unsigned checked = 0;
  for (unsigned index = 0; index < cases.size(); ++index) {
    for (unsigned flags = 0; flags < stop_disabled; ++flags) {
      const auto ccr = static_cast<Byte>(flags);
      auto expected_ram = image();
      reset(expected_ram, cases[index]);
      const auto expected = predict(expected_ram, ccr);
      guest(device, cases[index], ccr, expected);
      memory(device.ram, expected_ram);
      auto host_ram = image();
      native(host_ram, cases[index], ccr, expected);
      memory(host_ram, expected_ram);
      if (index < dft_cases && flags == 0) { check_dft(cases[index], device.ram); }
      ++checked;
    }
  }
  std::cout << "fft_independent_cases=" << checked << " points=" << points
            << " retired_per_case=" << retired << '\n';
}

template <class Operation> double median(Operation operation) {
  constexpr unsigned trials = 9;
  constexpr unsigned repetitions = 32768;
  std::array<double, trials> samples{};
  for (auto &sample : samples) {
    const auto start = Clock::now();
    for (unsigned index = 0; index < repetitions; ++index) { operation(); }
    sample = std::chrono::duration<double, std::nano>(Clock::now() - start).count() / repetitions;
  }
  std::ranges::sort(samples);
  return samples[trials / 2];
}

void measure(const std::string_view name, const Samples &input) {
  Device device;
  constexpr Byte ccr = retained_flags;
  auto expected_ram = image();
  reset(expected_ram, input);
  const auto expected = predict(expected_ram, ccr);
  const auto start = Clock::now();
  guest(device, input, ccr, expected);
  const auto cold = Clock::now() - start;
  const auto compiled = device.core.statistics().compiled_entries;
  const auto core_ns = median([&] { guest(device, input, ccr, expected); });
  memory(device.ram, expected_ram);
  auto host_ram = image();
  const auto host_ns = median([&] { native(host_ram, input, ccr, expected); });
  memory(host_ram, expected_ram);
  require(device.core.statistics().compiled_entries == compiled,
          "Warm FFT execution must retain its translation without recompilation");
  std::cout << "fft_case=" << name;
  test::bench::report(retired,
                      std::chrono::duration<double>(cold).count(),
                      core_ns / test::bench::nanoseconds_per_second,
                      host_ns / test::bench::nanoseconds_per_second);
  std::cout << " points=" << points
            << " cold_us=" << std::chrono::duration<double, std::micro>(cold).count()
            << " compiled_entries=" << compiled << " warm_scope=reset+run+verify"
            << " core_ns=" << core_ns << " host_asm_ns=" << host_ns << '\n';
}
}

int main() {
  try {
    validation();
    const auto cases = inputs();
    measure("impulse", cases[0]);
    measure("complex-tone", cases[2]);
    measure("noise", cases[3]);
    measure("word-wrap", cases.back());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
