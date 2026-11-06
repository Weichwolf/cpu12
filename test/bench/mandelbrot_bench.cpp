// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "jit/arena.hpp"
#include "jit/block.hpp"
#include "mandelbrot_host.hpp"
#include "mandelbrot_program.hpp"
#include "report.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <ratio>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {
using namespace cpu12;
using namespace cpu12::test::mandelbrot;
using Clock = std::chrono::steady_clock;
constexpr unsigned preserved = 0x70, prefix_instructions = 17, tail_instructions = 15;
constexpr unsigned trials = 9, repetitions = 32768;

int signed_word(const Address value) {
  return value < sign ? int{value} : int{value} - int{word_mask + 1};
}

Address multiply(Image &ram, const Address left, const Address right, Registers &state) {
  const auto full = std::int64_t{signed_word(left)} * signed_word(right);
  const auto bits = static_cast<std::uint32_t>(full);
  state.y = static_cast<Address>(bits >> 16U);
  word(ram, product, state.y);
  word(ram, product + 2, static_cast<Address>(bits & word_mask));
  // Independent arithmetic definition; negative Q8.8 products round down.
  constexpr std::int64_t scale = 256;
  const auto quotient = full >= 0 ? full / scale : -((-full + scale - 1) / scale);
  return static_cast<Address>(quotient);
}

Result predict(Image &ram, const Point point) {
  reset(ram, point);
  Result result{.registers = initial(point)};
  auto &state = result.registers;
  while (true) {
    const auto x_value = word(ram, x);
    const auto y_value = word(ram, y);
    const auto x_squared = multiply(ram, x_value, x_value, state);
    word(ram, xx, x_squared);
    const auto y_squared = multiply(ram, y_value, y_value, state);
    word(ram, yy, y_squared);
    const auto radius = (unsigned{x_squared} + y_squared) & word_mask;
    result.retired += prefix_instructions;
    if (radius >= radius_limit) {
      state.d = static_cast<Address>(radius);
      state.ccr = static_cast<Byte>((point.ccr & preserved) | (radius == radius_limit ? 4U : 0U));
      break;
    }
    const auto cross = multiply(ram, x_value, y_value, state);
    const auto doubled = (unsigned{cross} * 2U) & word_mask;
    word(ram, y, static_cast<Address>((doubled + point.imaginary) & word_mask));
    const auto difference = (unsigned{x_squared} + word_mask + 1U - y_squared) & word_mask;
    const auto sum = difference + point.real;
    state.d = static_cast<Address>(sum & word_mask);
    word(ram, x, state.d);
    --state.x;
    result.retired += tail_instructions;
    state.ccr = static_cast<Byte>((point.ccr & preserved) | (sum > word_mask ? 1U : 0U) |
                                  (state.d >= sign ? 8U : 0U) | (state.x == 0 ? 4U : 0U));
    if (state.x == 0) { break; }
  }
  ++result.retired; // Original STOP.
  state.pc = return_pc;
  state.sp = stack - 9;
  frame(ram, state);
  return result;
}

void verify(const State &state, const RunResult actual, const Result expected) {
  if (actual.fault != Fault::none || actual.retired != expected.retired ||
      actual.pc != stop_entry || actual.opcode != isa::stop[0] ||
      state.execution != ExecutionState::stopped || state.registers != expected.registers) {
    std::cerr << "fault=" << unsigned{static_cast<Byte>(actual.fault)}
              << " retired=" << actual.retired << "/" << expected.retired
              << " d=" << state.registers.d << "/" << expected.registers.d
              << " x=" << state.registers.x << "/" << expected.registers.x
              << " y=" << state.registers.y << "/" << expected.registers.y
              << " ccr=" << unsigned{state.registers.ccr} << "/" << unsigned{expected.registers.ccr}
              << '\n';
    throw std::runtime_error(
        "Mandelbrot guest disagrees with independent fixed-point expectations");
  }
}

void guest(Core &core, Image &ram, const Point point, const Result expected) {
  reset(ram, point);
  core.set_registers(initial(point));
  const auto result =
      core.run((unsigned{point.limit} * instructions_per_iteration) + prefix_instructions + 1);
  verify(core.state(), result, expected);
}

void native(Image &ram, const Point point, const Result expected) {
  reset(ram, point);
  const auto state = host(ram, point);
  if (state != expected.registers) {
    throw std::runtime_error("Host ASM disagrees with independent Mandelbrot state");
  }
  frame(ram, state);
}

void memory(const Image &ram, const Image &expected) {
  if (ram == expected) { return; }
  const auto mismatch = std::ranges::mismatch(ram, expected);
  std::cerr << "ram_address=" << (mismatch.in1 - ram.begin())
            << " actual=" << unsigned{*mismatch.in1} << " expected=" << unsigned{*mismatch.in2}
            << '\n';
  throw std::runtime_error("Mandelbrot changed unexpected RAM or original STOP frame bytes");
}

class NativeRun {
public:
  explicit NativeRun(const Memory &map) {
    const std::array entries{entry, tail_entry, stop_entry};
    for (const auto at : entries) {
      const auto code = detail::emit_block(detail::parse_block(map, at), map);
      code_size += code.size();
      page[at & page_mask] = arena.append(code);
    }
    index[entry >> detail::native_page_shift] = page.data();
  }

  void execute(Image &ram, const Point point, const Result expected) {
    reset(ram, point);
    context = {};
    context.cpu.registers = initial(point);
    const auto result = detail::enter_blocks(page[entry & page_mask],
                                             context.cpu.registers,
                                             context,
                                             std::numeric_limits<std::uint32_t>::max(),
                                             index);
    verify(context.cpu,
           {.retired = result.retired,
            .fault = context.fault,
            .pc = result.location.pc,
            .opcode = result.location.opcode},
           expected);
  }

  [[nodiscard]] std::size_t code_bytes() const { return code_size; }

private:
  std::size_t code_size = 0;
  static constexpr unsigned page_mask = detail::native_page_entries - 1;
  detail::ExecutableArena arena;
  detail::NativeContext context{};
  detail::NativePage page{};
  detail::NativeIndex index{};
};

template <class Operation> double median(Operation operation) {
  std::array<double, trials> samples{};
  for (auto &sample : samples) {
    const auto begin = Clock::now();
    for (unsigned index = 0; index < repetitions; ++index) { operation(); }
    sample = std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / repetitions;
  }
  std::ranges::sort(samples);
  return samples[trials / 2];
}

void validation_cases() {
  Image ram{};
  std::ranges::copy(program, ram.begin() + entry);
  const auto end = static_cast<Address>(entry + program.size());
  const std::array regions{Ram{.bytes = std::span(ram).first(entry)},
                           Ram{.first = entry,
                               .bytes = std::span(ram).subspan(entry, program.size()),
                               .writable = false},
                           Ram{.first = end, .bytes = std::span(ram).subspan(end)}};
  Core core(Memory::mapped(regions));
  NativeRun direct(Memory::mapped(regions));
  constexpr std::array<Address, 7> real_values{0xfe00, 0xff00, 0xff40, 0xffff, 0, 0x40, 0x100};
  constexpr std::array<Address, 6> imaginary_values{0xff00, 0xffff, 0, 0x20, 0x80, 0x100};
  constexpr std::array<Address, 4> limits{1, 2, 8, 32};
  constexpr std::array<Byte, 2> conditions{0, preserved_flags};
  unsigned cases = 0;
  for (const auto real : real_values) {
    for (const auto imaginary : imaginary_values) {
      for (const auto limit : limits) {
        for (const auto ccr : conditions) {
          const Point point{.real = real, .imaginary = imaginary, .limit = limit, .ccr = ccr};
          auto expected_ram = ram;
          const auto expected = predict(expected_ram, point);
          guest(core, ram, point, expected);
          memory(ram, expected_ram);
          direct.execute(ram, point, expected);
          memory(ram, expected_ram);
          auto host_ram = ram;
          native(host_ram, point, expected);
          memory(host_ram, expected_ram);
          ++cases;
        }
      }
    }
  }
  std::cout << "mandelbrot_independent_cases=" << cases << '\n';
}

void measure(const std::string_view name, const Point point) {
  Image ram{};
  std::ranges::copy(program, ram.begin() + entry);
  auto expected_ram = ram;
  const auto expected = predict(expected_ram, point);
  const auto end = static_cast<Address>(entry + program.size());
  const std::array regions{Ram{.bytes = std::span(ram).first(entry)},
                           Ram{.first = entry,
                               .bytes = std::span(ram).subspan(entry, program.size()),
                               .writable = false},
                           Ram{.first = end, .bytes = std::span(ram).subspan(end)}};
  Core core(Memory::mapped(regions));
  const auto begin = Clock::now();
  guest(core, ram, point, expected);
  const auto cold = Clock::now() - begin;
  memory(ram, expected_ram);
  NativeRun direct(Memory::mapped(regions));
  direct.execute(ram, point, expected);
  memory(ram, expected_ram);
  auto host_ram = ram;
  native(host_ram, point, expected);
  memory(host_ram, expected_ram);
  const auto compiled = core.statistics().compiled_entries;
  const auto core_ns = median([&] { guest(core, ram, point, expected); });
  const auto direct_ns = median([&] { direct.execute(ram, point, expected); });
  const auto host_ns = median([&] { native(host_ram, point, expected); });
  memory(ram, expected_ram);
  memory(host_ram, expected_ram);
  if (core.statistics().compiled_entries != compiled) {
    throw std::runtime_error("Warm Mandelbrot translated new entries");
  }
  std::cout << name;
  test::bench::report(expected.retired,
                      std::chrono::duration<double>(cold).count(),
                      core_ns / test::bench::nanoseconds_per_second,
                      host_ns / test::bench::nanoseconds_per_second);
  std::cout << " retired=" << expected.retired
            << " cold_run_us=" << std::chrono::duration<double, std::micro>(cold).count()
            << " core_ns=" << core_ns << " host_asm_ns=" << host_ns
            << " native_entries_to_host_percent=" << (100.0 * host_ns) / direct_ns
            << " native_code_bytes=" << direct.code_bytes() << '\n';
}
}

int main() {
  try {
    validation_cases();
    measure("mandelbrot-zero", {.real = 0, .imaginary = 0});
    constexpr Point period_two{.real = 0xff00, .imaginary = 0};
    constexpr Point boundary{.real = 0xff40, .imaginary = 0x20};
    measure("mandelbrot-period-two", period_two);
    measure("mandelbrot-boundary", boundary);
    measure("mandelbrot-escape", {.real = 0x100, .imaginary = 0x100});
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
