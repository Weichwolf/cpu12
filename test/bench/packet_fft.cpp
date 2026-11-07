// SPDX-License-Identifier: GPL-3.0-only
#include "packet_fft.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fft_host.hpp"
#include "fft_oracle.hpp"
#include "fft_program.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>

namespace cpu12::test::packet_fft {
namespace {
constexpr unsigned words_per_frame = fft::points * 2;
constexpr auto input_bytes = std::size_t{fft::points} * 4;
constexpr auto transform_bytes = fft::program.size() - isa::stop.size();
constexpr auto prefix_bytes = (words_per_frame * 6) + 6;
constexpr auto program_bytes = prefix_bytes + transform_bytes + 3;
constexpr Address jump_pc = fft::entry + program_bytes - 3, code_end = fft::entry + program_bytes;
static_assert(code_end < fft::data);

consteval auto assemble() {
  std::array<Byte, program_bytes> result{};
  std::size_t offset = 0;
  const auto append = [&](const auto &bytes) {
    for (const auto byte : bytes) { result[offset++] = byte; }
  };
  append(isa::program(isa::opcode<isa::Opcode::ldd_extended>(), fft::address(seed_address)));
  for (unsigned sample = 0; sample < fft::points; ++sample) {
    unsigned reversed = 0;
    for (unsigned bit = 0; bit < fft::stages; ++bit) {
      reversed = (reversed << 1U) | ((sample >> bit) & 1U);
    }
    for (unsigned component = 0; component < 2; ++component) {
      const auto destination = static_cast<Address>(fft::data + (reversed * 4) + (component * 2));
      append(isa::program(isa::opcode<isa::Opcode::addd_immediate>(),
                          fft::address(input_stride),
                          isa::opcode<isa::Opcode::std_extended>(),
                          fft::address(destination)));
    }
  }
  append(isa::program(isa::opcode<isa::Opcode::std_extended>(), fft::address(seed_address)));
  for (std::size_t index = 0; index < transform_bytes; ++index) {
    result[offset++] = fft::program[index];
  }
  append(isa::program(isa::opcode<isa::Opcode::jmp_extended>(), fft::address(fft::entry)));
  if (offset != result.size()) { throw "Authored FFT stream size"; }
  return result;
}

constexpr auto program = assemble();

Address host_input(const std::span<Byte, input_bytes> destination, Address sum) {
#if defined(__aarch64__)
  std::uint32_t value;
#endif
  // Match the guest's unrolled, bit-reversed stores without restoring its state.
  asm volatile(
#if defined(__aarch64__)
      "mov w10,#%c[stride]\n\t"
      ".macro cpu12_stream_sample offset\n\t"
      "add %w[sum],%w[sum],w10\n\t"
      "uxth %w[sum],%w[sum]\n\t"
      "rev16 w9,%w[sum]\n\t"
      "strh w9,[%[data],#\\offset]\n\t"
#else
      ".macro cpu12_stream_sample offset\n\t"
      "addw $%c[stride], %%ax\n\t"
#if defined(__MOVBE__)
      "movbew %%ax, \\offset(%%rdi)\n\t"
#else
      "movb %%ah, \\offset(%%rdi)\n\t"
      "movb %%al, (\\offset+1)(%%rdi)\n\t"
#endif
#endif
      ".endm\n\t"
      R"(
    cpu12_stream_sample 0
    cpu12_stream_sample 2
    cpu12_stream_sample 32
    cpu12_stream_sample 34
    cpu12_stream_sample 16
    cpu12_stream_sample 18
    cpu12_stream_sample 48
    cpu12_stream_sample 50
    cpu12_stream_sample 8
    cpu12_stream_sample 10
    cpu12_stream_sample 40
    cpu12_stream_sample 42
    cpu12_stream_sample 24
    cpu12_stream_sample 26
    cpu12_stream_sample 56
    cpu12_stream_sample 58
    cpu12_stream_sample 4
    cpu12_stream_sample 6
    cpu12_stream_sample 36
    cpu12_stream_sample 38
    cpu12_stream_sample 20
    cpu12_stream_sample 22
    cpu12_stream_sample 52
    cpu12_stream_sample 54
    cpu12_stream_sample 12
    cpu12_stream_sample 14
    cpu12_stream_sample 44
    cpu12_stream_sample 46
    cpu12_stream_sample 28
    cpu12_stream_sample 30
    cpu12_stream_sample 60
    cpu12_stream_sample 62
    .purgem cpu12_stream_sample
  )"
#if defined(__aarch64__)
      : [sum] "=&r"(value)
      : "0"(std::uint32_t{sum}), [data] "r"(destination.data()), [stride] "i"(input_stride)
      : "cc", "memory", "x9", "x10");
  return static_cast<Address>(value);
#else
      : "+a"(sum)
      : "D"(destination.data()), [stride] "i"(input_stride)
      : "cc", "memory");
  return sum;
#endif
}
}

fft::Image image(const Case point) {
  fft::Image ram;
  constexpr Byte sentinel = 0xa5;
  ram.fill(sentinel);
  std::ranges::copy(program, ram.begin() + fft::entry);
  constexpr std::array<int, fft::points> coefficients{
      256, 0, 237, -98, 181, -181, 98, -237, 0, -256, -98, -237, -181, -181, -237, -98};
  for (unsigned index = 0; index < coefficients.size(); ++index) {
    fft::word(
        ram, static_cast<Address>(fft::twiddles + (index * 2)), fft::narrow(coefficients[index]));
  }
  fft::word(ram, seed_address, point.seed);
  return ram;
}

std::array<Ram, 5> regions(fft::Image &ram) {
  constexpr auto coefficients_end = fft::twiddles + (fft::points * 2);
  return {
      Ram{.bytes = std::span(ram).first(fft::entry)},
      Ram{.first = fft::entry,
          .bytes = std::span(ram).subspan(fft::entry, program_bytes),
          .writable = false},
      Ram{.first = code_end, .bytes = std::span(ram).subspan(code_end, fft::twiddles - code_end)},
      Ram{.first = fft::twiddles,
          .bytes = std::span(ram).subspan(fft::twiddles, std::size_t{fft::points} * 2),
          .writable = false},
      Ram{.first = coefficients_end, .bytes = std::span(ram).subspan(coefficients_end)}};
}

void verify(const fft::Image &ram,
            const Registers registers,
            const Case point,
            const std::uint64_t frames) {
  auto wanted = image(point);
  auto state = fft::initial(point.ccr);
  if (frames != 0) {
    fft::Samples input;
    const auto before =
        (point.seed + ((frames - 1) * words_per_frame * input_stride)) & fft::word_mask;
    for (unsigned index = 0; index < input.size(); ++index) {
      input[index] = static_cast<Address>((before + ((std::uint64_t{index} + 1) * input_stride)) &
                                          fft::word_mask);
    }
    fft::reset(wanted, input);
    fft::word(wanted, seed_address, input.back());
    state = fft::predict_transform(wanted, state);
  }
  if (ram != wanted || registers != state) {
    throw std::runtime_error(
        "Continuous FFT stream differs from independent input/transform state and memory");
  }
}

Guest::Guest(const Case point)
    : point_(point), ram_(image(point)), regions_(regions(ram_)), core_(Memory::mapped(regions_)) {
  core_.set_registers(fft::initial(point.ccr));
}

void Guest::advance(const unsigned frames) {
  const auto instructions = frames * instructions_per_frame;
  const auto result = core_.run(instructions);
  const RunResult wanted{.retired = instructions,
                         .pc = jump_pc,
                         .opcode = isa::opcode<isa::Opcode::jmp_extended>()[0]};
  if (result != wanted) {
    throw std::runtime_error("FFT stream must retire the original guest loop exactly");
  }
  retired_ += result.retired;
  frames_ += frames;
}

void Guest::verify() const {
  packet_fft::verify(ram_, core_.registers(), point_, frames_);
  const auto wanted = State{.registers = core_.registers(),
                            .failure = {},
                            .execution = ExecutionState::running,
                            .irq_delay = 0,
                            .returned_frame = false};
  if (core_.state() != wanted || retired_ != frames_ * instructions_per_frame) {
    throw std::runtime_error("FFT stream execution state/progress differs");
  }
}

std::uint64_t Guest::compiled() const noexcept {
  return core_.statistics().compiled_entries;
}

Host::Host(const Case point)
    : point_(point), ram_(image(point)), registers_(fft::initial(point.ccr)) {}

void Host::advance(const unsigned frames) {
  for (unsigned frame = 0; frame < frames; ++frame) {
    fft::word(ram_,
              seed_address,
              host_input(std::span(ram_).subspan<fft::data, input_bytes>(),
                         fft::word(ram_, seed_address)));
    registers_ = fft::host_transform(ram_, registers_);
  }
  frames_ += frames;
}

void Host::verify() const {
  packet_fft::verify(ram_, registers_, point_, frames_);
}

void verify_cases() {
  constexpr std::array<Address, 6> seeds{0, 1, 0x7fff, 0x8000, 0xff00, 0xffff};
  constexpr std::array<Byte, 4> flags{0, 0x50, 0x80, 0xff};
  for (const auto seed : seeds) {
    for (const auto ccr : flags) {
      const Case point{.seed = seed, .ccr = ccr};
      Guest guest(point);
      Host host(point);
      guest.verify();
      host.verify();
      for (const auto frames : {1U, 2U, channels, 3U}) {
        guest.advance(frames);
        host.advance(frames);
        guest.verify();
        host.verify();
      }
    }
  }
  std::cout << "continuous_fft_cases=" << seeds.size() * flags.size()
            << " full_state_memory=PASS\n";
}
}
