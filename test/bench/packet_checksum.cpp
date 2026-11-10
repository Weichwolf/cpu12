// SPDX-License-Identifier: GPL-3.0-only
#include "packet_checksum.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "word_checksum_host.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>

namespace cpu12::test::packet_checksum {
namespace {
constexpr Byte sentinel = 0xa5, negative = 8, zero = 4;
constexpr std::array<Byte, 1> postincrement_x{0x31}, decrement_y{0x36}, word_backedge{0xf5};
constexpr std::array<Byte, 2> sum_address{accumulator >> byte_bits, accumulator &byte_mask};
constexpr auto body = isa::program(isa::opcode<isa::Opcode::ldd_indexed>(),
                                   postincrement_x,
                                   isa::opcode<isa::Opcode::addd_extended>(),
                                   sum_address,
                                   isa::opcode<isa::Opcode::std_extended>(),
                                   sum_address,
                                   isa::opcode<isa::Opcode::dbne_relative>(),
                                   decrement_y,
                                   word_backedge);
constexpr unsigned prefix_bytes = 6, branch_bytes = 2, instructions_per_word = 4;
// S12CPUV2 nominal timing: four three-cycle body operations, two two-cycle
// immediate loads, then the three-cycle packet backedge.
constexpr unsigned cycles_per_word = 12, cycles_per_packet_setup = 7;
constexpr auto code_bytes = prefix_bytes + body.size() + branch_bytes;
constexpr Address code_end = entry + code_bytes, branch_pc = code_end - branch_bytes;
static_assert(code_bytes < 128);

auto program(const Address words) {
  constexpr std::array<Byte, 2> count{0, 0};
  constexpr std::array<Byte, 2> input_address{input >> byte_bits, input & byte_mask};
  constexpr std::array<Byte, 1> packet_backedge{static_cast<Byte>(256 - code_bytes)};
  auto code = isa::program(isa::opcode<isa::Opcode::ldx_immediate>(),
                           input_address,
                           isa::opcode<isa::Opcode::ldy_immediate>(),
                           count,
                           body,
                           isa::opcode<isa::Opcode::bra_relative>(),
                           packet_backedge);
  constexpr unsigned count_operand = 4;
  code[count_operand] = static_cast<Byte>(words >> byte_bits);
  code[count_operand + 1] = static_cast<Byte>(words & byte_mask);
  return code;
}

void word(Image &ram, const Address address, const Address value) {
  ram[address] = static_cast<Byte>(value >> byte_bits);
  ram[address + 1U] = static_cast<Byte>(value & byte_mask);
}

unsigned word(const Image &ram, const Address address) {
  return (unsigned{ram[address]} << byte_bits) | ram[address + 1U];
}

struct Sum {
  Address value = 0;
  Byte carry = 0;
};

Sum predict(const Case point, const std::uint64_t packets) {
  const auto initial = image(point);
  std::uint64_t total = 0;
  for (unsigned index = 0; index < point.words; ++index) {
    total += word(initial, static_cast<Address>(input + (index * word_bytes)));
  }
  const auto last = word(initial, static_cast<Address>(input + ((point.words - 1U) * word_bytes)));
  const auto final = (point.seed + (packets * total)) & word_mask;
  const auto before_last = (final + word_mask + 1 - last) & word_mask;
  return {.value = static_cast<Address>(final),
          .carry = static_cast<Byte>(packets != 0 && before_last + last > word_mask)};
}

auto regions(Image &ram, const Case point) {
  const auto input_bytes = std::size_t{point.words} * word_bytes;
  const auto input_end = static_cast<Address>(input + input_bytes);
  return std::array{
      Ram{.bytes = std::span(ram).first(entry)},
      Ram{.first = entry, .bytes = std::span(ram).subspan(entry, code_bytes), .writable = false},
      Ram{.first = code_end, .bytes = std::span(ram).subspan(code_end, input - code_end)},
      Ram{.first = input, .bytes = std::span(ram).subspan(input, input_bytes), .writable = false},
      Ram{.first = input_end, .bytes = std::span(ram).subspan(input_end)}};
}
}

std::uint32_t instructions(const Case point) {
  return (unsigned{point.words} * instructions_per_word) + 3;
}

Image image(const Case point) {
  if (point.words == 0 || point.words > maximum_words) {
    throw std::invalid_argument("Packet checksum word count exceeds the authored input window");
  }
  Image ram;
  ram.fill(sentinel);
  const auto code = program(point.words);
  std::ranges::copy(code, ram.begin() + entry);
  constexpr unsigned stride = 257;
  constexpr unsigned bias = 0x7fff;
  for (unsigned index = 0; index < point.words; ++index) {
    word(ram,
         static_cast<Address>(input + (index * word_bytes)),
         static_cast<Address>(((index * stride) + bias) & word_mask));
  }
  word(ram, accumulator, point.seed);
  return ram;
}

Registers expected(const Case point, const std::uint64_t packets) {
  if (packets == 0) { return {.pc = entry, .sp = stack, .ccr = point.ccr}; }
  const auto sum = predict(point, packets);
  return {.pc = entry,
          .d = sum.value,
          .x = static_cast<Address>(input + (unsigned{point.words} * word_bytes)),
          .y = 0,
          .sp = stack,
          .ccr = static_cast<Byte>((point.ccr & retained_flags) | sum.carry |
                                   (sum.value == 0 ? zero : 0U) |
                                   (sum.value >= 0x8000 ? negative : 0U))};
}

void verify_memory(const Image &ram, const Case point, const std::uint64_t packets) {
  auto wanted = image(point);
  word(wanted, accumulator, predict(point, packets).value);
  if (ram != wanted) { throw std::runtime_error("Continuous checksum changed unexpected RAM"); }
}

Guest::Guest(const Case point)
    : point_(point),
      ram_(image(point)),
      regions_(regions(ram_, point)),
      core_(Memory::mapped(regions_)) {
  core_.set_registers(expected(point, 0));
}

void Guest::packet() {
  const auto cycles = (std::uint64_t{point_.words} * cycles_per_word) + cycles_per_packet_setup;
  const auto result = core_.run_until(core_.progress().estimated_cycles + cycles);
  const RunResult wanted{.retired = instructions(point_),
                         .pc = branch_pc,
                         .opcode = isa::opcode<isa::Opcode::bra_relative>()[0]};
  if (result != wanted) {
    throw std::runtime_error("Continuous checksum packet did not retire exactly");
  }
  retired_ += result.retired;
  ++packets_;
}

void Guest::verify() const {
  const State wanted{.registers = expected(point_, packets_),
                     .failure = {},
                     .execution = ExecutionState::running,
                     .irq_delay = 0,
                     .returned_frame = false};
  const auto cycles = (std::uint64_t{point_.words} * cycles_per_word) + cycles_per_packet_setup;
  if (core_.state() != wanted || retired_ != packets_ * instructions(point_) ||
      core_.progress().estimated_cycles != packets_ * cycles) {
    throw std::runtime_error(
        "Continuous checksum state/progress differs from independent recurrence");
  }
  verify_memory(ram_, point_, packets_);
}

std::uint64_t Guest::compiled() const noexcept {
  return core_.statistics().compiled_entries;
}

Host::Host(const Case point, const bool cpp)
    : point_(point), ram_(image(point)), result_{.sum = point.seed, .carry = 0}, cpp_(cpp) {}

void Host::packet() {
  const auto operation = cpp_ ? checksum::host_cpp : checksum::host;
  result_ = operation(std::span(ram_).subspan(input, std::size_t{point_.words} * word_bytes),
                      std::span<Byte, 2>(&ram_[accumulator], 2),
                      result_.sum);
  ++packets_;
}

void Host::verify() const {
  const auto wanted = predict(point_, packets_);
  if (result_.sum != wanted.value || result_.carry != wanted.carry) {
    throw std::runtime_error("Continuous host sum/carry differs from independent recurrence");
  }
  verify_memory(ram_, point_, packets_);
}

void verify_cases() {
  constexpr std::array<Address, 5> counts{1, 2, 31, 256, maximum_words};
  constexpr std::array<Address, 4> seeds{0, 0x7fff, 0x8001, 0xffff};
  constexpr std::array<Byte, 3> flags{0, 0x50, 0xff};
  unsigned checked = 0;
  for (const auto count : counts) {
    for (const auto seed : seeds) {
      for (const auto ccr : flags) {
        const Case point{.words = count, .seed = seed, .ccr = ccr};
        Guest guest(point);
        Host host(point);
        CppHost cpp(point);
        guest.verify();
        host.verify();
        cpp.verify();
        for (unsigned packet = 0; packet < 3; ++packet) {
          guest.packet();
          host.packet();
          cpp.packet();
          guest.verify();
          host.verify();
          cpp.verify();
        }
        ++checked;
      }
    }
  }
  std::cout << "continuous_checksum_cases=" << checked << " full_state_memory=PASS\n";
}
}
