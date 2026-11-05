// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "word_checksum_host.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <ratio>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
using Image = std::array<Byte, address_bytes>;
using Clock = std::chrono::steady_clock;
constexpr Address entry = 0x0200, input = 0x4000, accumulator = 0x6000, stack = 0x9000;
constexpr Address maximum_count = 256;
constexpr unsigned word_mask = 0xffff, byte_bits = 8, byte_mask = 0xff;
constexpr Byte negative = 8, zero = 4, preserved_flags = 0x70;
constexpr Byte sentinel = 0xa5;
// X postincrement by two; DBNE Y with a negative signed-nine-bit displacement.
constexpr std::array<Byte, 1> x_postincrement_two{0x31}, dbne_y_backward{0x36};
constexpr std::array<Byte, 2> accumulator_operand{accumulator >> byte_bits, accumulator &byte_mask};
constexpr auto body = isa::program(isa::opcode<isa::Opcode::ldd_indexed>(),
                                   x_postincrement_two,
                                   isa::opcode<isa::Opcode::addd_extended>(),
                                   accumulator_operand,
                                   isa::opcode<isa::Opcode::std_extended>(),
                                   accumulator_operand,
                                   isa::opcode<isa::Opcode::dbne_relative>(),
                                   dbne_y_backward);
constexpr std::array<Byte, 1> backedge{static_cast<Byte>(0x100U - (body.size() + 1U))};
constexpr auto program = isa::program(body, backedge, isa::stop);
constexpr Address stop_pc = entry + body.size() + backedge.size();
constexpr Address return_pc = entry + program.size();
constexpr Address frame_bytes = 9;

void word(Image &ram, const Address address, const Address value) {
  ram[address] = static_cast<Byte>(value >> byte_bits);
  ram[address + 1U] = static_cast<Byte>(value & byte_mask);
}

Address word(const Image &ram, const Address address) {
  return static_cast<Address>((unsigned{ram[address]} << byte_bits) | ram[address + 1U]);
}

struct Case {
  Address count = maximum_count;
  Address seed = 0;
  Byte ccr = 0;
};

Registers initial(const Case point) {
  return {.pc = entry, .x = input, .y = point.count, .sp = stack, .ccr = point.ccr};
}

struct Expected {
  Registers registers;
  Image ram;
};

Expected predict(const Image &before, const Case point) {
  Expected result{.registers = initial(point), .ram = before};
  unsigned sum = point.seed;
  unsigned carry = 0;
  for (unsigned index = 0; index < point.count; ++index) {
    const auto wide = sum + word(before, static_cast<Address>(input + (index * 2U)));
    sum = wide & word_mask;
    carry = wide >> 16U;
  }
  const auto ccr = static_cast<Byte>((point.ccr & preserved_flags) | carry |
                                     (sum == 0 ? zero : 0U) | (sum >= 0x8000U ? negative : 0U));
  const auto end = static_cast<Address>(input + (point.count * 2U));
  result.registers = {.pc = return_pc,
                      .d = static_cast<Address>(sum),
                      .x = end,
                      .y = 0,
                      .sp = stack - frame_bytes,
                      .ccr = ccr};
  word(result.ram, accumulator, static_cast<Address>(sum));
  const std::array<Byte, frame_bytes> frame{ccr,
                                            static_cast<Byte>(sum & byte_mask),
                                            static_cast<Byte>(sum >> byte_bits),
                                            static_cast<Byte>(end >> byte_bits),
                                            static_cast<Byte>(end & byte_mask),
                                            0,
                                            0,
                                            static_cast<Byte>(return_pc >> byte_bits),
                                            static_cast<Byte>(return_pc & byte_mask)};
  std::ranges::copy(frame, result.ram.begin() + stack - frame_bytes);
  return result;
}

void guest(Core &core, Image &ram, const Case point, const Registers expected) {
  word(ram, accumulator, point.seed);
  core.set_registers(initial(point));
  const auto result = core.run((point.count * 4U) + 1U);
  if (result.fault != Fault::none || result.retired != point.count * 4U + 1U ||
      result.pc != stop_pc || result.opcode != isa::stop[0] ||
      core.state().execution != ExecutionState::stopped || core.registers() != expected) {
    throw std::runtime_error("Checksum guest disagrees with independent state expectations");
  }
}

void native(Image &ram, const Case point, const Registers expected) {
  word(ram, accumulator, point.seed);
  const auto result =
      test::checksum::host(std::span(ram).subspan(input, std::size_t{point.count} * 2U),
                           std::span<Byte, 2>(&ram[accumulator], 2),
                           point.seed);
  const auto ccr =
      static_cast<Byte>((point.ccr & preserved_flags) | result.carry |
                        (result.sum == 0 ? zero : 0U) | (result.sum >= 0x8000U ? negative : 0U));
  if (result.sum != expected.d || ccr != expected.ccr || word(ram, accumulator) != expected.d) {
    throw std::runtime_error("Checksum host ASM disagrees with independent sum/carry/flags");
  }
  // Actual STOP writes all nine bytes; include those writes in both timed paths.
  word(ram, stack - 2, return_pc);
  word(ram, stack - 4, 0);
  word(ram, stack - 6, expected.x);
  ram[stack - 8] = static_cast<Byte>(result.sum & byte_mask);
  ram[stack - 7] = static_cast<Byte>(result.sum >> byte_bits);
  ram[stack - 9] = ccr;
}

template <class Operation> double median(Operation operation) {
  constexpr unsigned trials = 9;
  constexpr unsigned repetitions = 64;
  std::array<double, trials> times{};
  for (auto &sample : times) {
    const auto begin = Clock::now();
    for (unsigned repetition = 0; repetition < repetitions; ++repetition) { operation(); }
    sample = std::chrono::duration<double, std::nano>(Clock::now() - begin).count() / repetitions;
  }
  std::ranges::sort(times);
  return times[trials / 2];
}

void exercise() {
  Image ram;
  ram.fill(sentinel);
  std::ranges::copy(program, ram.begin() + entry);
  for (unsigned index = 0; index < maximum_count; ++index) {
    const auto value = static_cast<Address>((index * 257U + 0x7fffU) & word_mask);
    word(ram, static_cast<Address>(input + (index * 2U)), value);
  }
  const auto code_end = static_cast<Address>(entry + program.size());
  const std::array regions{Ram{.bytes = std::span(ram).first(entry)},
                           Ram{.first = entry,
                               .bytes = std::span(ram).subspan(entry, program.size()),
                               .writable = false},
                           Ram{.first = code_end, .bytes = std::span(ram).subspan(code_end)}};
  Core core(Memory::mapped(regions));
  const auto before = ram;
  const auto cold_expected = predict(before, Case{});
  const auto cold_start = Clock::now();
  guest(core, ram, Case{}, cold_expected.registers);
  const auto cold = Clock::now() - cold_start;
  const auto compiled_entries = core.statistics().compiled_entries;
  constexpr std::array<Address, 4> counts{1, 2, 31, maximum_count};
  constexpr std::array<Address, 6> seeds{0, 1, 0x7fff, 0x8000, 0x8001, 0xffff};
  for (const auto count : counts) {
    for (const auto seed : seeds) {
      for (unsigned flags = 0; flags <= preserved_flags; flags += 16U) {
        const Case point{.count = count, .seed = seed, .ccr = static_cast<Byte>(flags)};
        const auto expected = predict(before, point);
        ram = before;
        guest(core, ram, point, expected.registers);
        auto host_ram = before;
        native(host_ram, point, expected.registers);
        if (ram != expected.ram || host_ram != expected.ram) {
          throw std::runtime_error("Checksum changed unexpected RAM or STOP frame bytes");
        }
      }
    }
  }
  const Case point{};
  const auto expected = predict(before, point);
  auto host_ram = before;
  const auto core_ns = median([&] { guest(core, ram, point, expected.registers); });
  const auto host_ns = median([&] { native(host_ram, point, expected.registers); });
  if (ram != expected.ram || host_ram != expected.ram ||
      core.statistics().compiled_entries != compiled_entries) {
    throw std::runtime_error("Warm checksum effects differ or execution translated new entries");
  }
  std::cout << "indexed-word-checksum cases=" << counts.size() * seeds.size() * 8U
            << " words=" << maximum_count
            << " cold_run_us=" << std::chrono::duration<double, std::micro>(cold).count()
            << " compiled_entries=" << compiled_entries
            << " warm_scope=reset+run+verify core_ns=" << core_ns << " host_asm_ns=" << host_ns
            << " core_to_host_percent=" << 100.0 * host_ns / core_ns << '\n';
}
}

int main() {
  try {
    exercise();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
