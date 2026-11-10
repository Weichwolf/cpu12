// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpu12::test::prepared_registers {
constexpr Address entry = 0x4000, stride = 8;
constexpr unsigned byte_mask = 0xff, byte_bits = 8;
constexpr Registers base{.d = 0xa55a, .x = 0x1234, .y = 0x5678, .sp = 0x9000};

struct Encoding {
  std::array<Byte, 2> bytes{};
  std::size_t length = 0;
};

template <isa::Opcode Op> consteval Encoding code() {
  constexpr auto bytes = isa::opcode<Op>();
  Encoding result{.length = bytes.size()};
  std::ranges::copy(bytes, result.bytes.begin());
  return result;
}

inline void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

struct Program {
  explicit Program(const std::span<const Encoding> codes) : bytes(codes.size() * stride) {
    entries.reserve(codes.size());
    for (const auto instruction : codes) {
      const auto offset = entries.size() * stride;
      entries.push_back(static_cast<Address>(entry + offset));
      std::ranges::copy(std::span(instruction.bytes).first(instruction.length),
                        bytes.begin() + static_cast<std::ptrdiff_t>(offset));
      bytes[offset + instruction.length] = static_cast<Byte>(isa::Opcode::bra_relative);
      bytes[offset + instruction.length + 1] =
          static_cast<Byte>(byte_mask - instruction.length - 1);
    }
  }

  std::vector<Byte> bytes;
  std::vector<Address> entries;
};

struct Pair {
  explicit Pair(const std::span<const Encoding> codes)
      : program(codes),
        original(program.bytes),
        prepared(Memory::window({.first = entry, .bytes = program.bytes, .writable = false})),
        native(Memory::window({.first = entry, .bytes = program.bytes, .writable = false})) {
    const auto state = prepared.state();
    const auto report = prepared.prepare(program.entries);
    require(report.prepared_entries == codes.size() * 2 && report.unresolved_entries == 0 &&
                report.native_bytes > 0 && report.failure.fault == Fault::none &&
                prepared.state() == state && prepared.progress() == Progress{},
            "Every register operation and backedge must prepare through LLVM without execution");
    compilations = prepared.statistics().compiled_entries;
  }

  void unchanged() const {
    require(program.bytes == original, "Prepared register execution never alters its source");
    if (prepared.statistics().compiled_entries != compilations) {
      throw std::runtime_error(
          "Prepared register source=" + std::to_string(original[0]) +
          " compiled_before=" + std::to_string(compilations) +
          " compiled_after=" + std::to_string(prepared.statistics().compiled_entries));
    }
  }

  Program program;
  std::vector<Byte> original;
  Core prepared, native;
  std::uint64_t compilations = 0;
};

inline void check(Pair &pair,
                  const Registers seed,
                  const Encoding encoding,
                  const State &expected,
                  const unsigned cycles) {
  pair.prepared.set_registers(seed);
  pair.native.set_registers(seed);
  const auto before = pair.prepared.progress();
  auto wanted = expected.failure;
  if (wanted.fault == Fault::none) {
    wanted = {.retired = 1, .pc = seed.pc, .opcode = encoding.bytes[0]};
  }
  const auto actual = pair.prepared.run(1);
  if (actual != wanted || pair.prepared.state() != expected ||
      pair.prepared.progress().estimated_cycles != before.estimated_cycles + cycles) {
    throw std::runtime_error("LLVM register opcode=" + std::to_string(encoding.bytes[0]) +
                             " D=" + std::to_string(seed.d) + " CCR=" + std::to_string(seed.ccr));
  }
  require(pair.native.run(1) == actual && pair.native.state() == expected &&
              pair.native.progress() == pair.prepared.progress(),
          "Independently checked LLVM register effects also match the qualified native path");
}

inline void prefixes(Pair &pair, const Registers seed) {
  constexpr std::array<unsigned, 8> grants{0, 1, 2, 3, 4, 5, 16, 17};
  for (const auto grant : grants) {
    pair.prepared.set_registers(seed);
    pair.native.set_registers(seed);
    require(pair.prepared.run(grant) == pair.native.run(grant) &&
                pair.prepared.state() == pair.native.state() &&
                pair.prepared.progress() == pair.native.progress(),
            "Register loops preserve every complete instruction prefix");
    pair.prepared.set_registers(seed);
    pair.native.set_registers(seed);
    const auto deadline = pair.prepared.progress().estimated_cycles + grant;
    require(pair.prepared.run_until(deadline) == pair.native.run_until(deadline) &&
                pair.prepared.state() == pair.native.state() &&
                pair.prepared.progress() == pair.native.progress(),
            "Register loops preserve atomic deadlines and interrupt-return boundaries");
  }
}

std::size_t unary_cases();
std::size_t ccr_cases();
std::size_t accumulator_cases();
}
