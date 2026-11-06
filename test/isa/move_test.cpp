// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "move_model.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
using namespace cpu12::test::move;
constexpr std::array<Registers, 3> seeds{
    {seed,
     {.pc = 0, .d = 0x7f80, .x = 0xffff, .y = 0, .sp = 0x8000, .ccr = 0x5a},
     {.pc = 0x7fff, .d = 0x10, .x = 0x8000, .y = 0xffff, .sp = 1, .ccr = 0xa5}}};
constexpr Operand x{.mode = Mode::indexed};
constexpr Operand y{.mode = Mode::indexed, .index = {.base = &Registers::y, .postbyte = 0x40}};
constexpr Operand source_extended{.mode = Mode::extended, .address = 0xffff};
constexpr Operand destination_extended{.mode = Mode::extended, .address = 0};

void one(ObservedCore &core, const std::span<Byte> ram, const Point point) {
  const auto wanted = plan(point);
  const unsigned width = point.word ? 2 : 1;
  const auto low_source = static_cast<std::uint16_t>(wanted.source + 1U);
  const auto low_destination = static_cast<std::uint16_t>(wanted.destination + 1U);
  const auto left = static_cast<std::uint16_t>(wanted.destination + 0xffffU);
  const auto right = static_cast<std::uint16_t>(wanted.destination + width);
  constexpr Byte first = 0xa5;
  constexpr Byte second = 0x5a;
  ram[wanted.destination] = first;
  ram[low_destination] = second;
  ram[left] = second;
  ram[right] = first;
  if (point.source.mode != Mode::immediate) {
    ram[wanted.source] = static_cast<Byte>(point.word ? point.data >> byte_bits : point.data);
    if (point.word) { ram[low_source] = static_cast<Byte>(point.data); }
  }
  const std::array watched{
      wanted.source, low_source, wanted.destination, low_destination, left, right};
  std::array<Byte, watched.size()> before{};
  for (unsigned i = 0; i < watched.size(); ++i) { before[i] = ram[watched[i]]; }
  retired(core, point, wanted);
  for (unsigned i = 0; i < watched.size(); ++i) {
    auto data = before[i];
    if (watched[i] == wanted.destination) {
      data = static_cast<Byte>(point.word ? point.data >> byte_bits : point.data);
    } else if (point.word && watched[i] == low_destination) {
      data = static_cast<Byte>(point.data);
    }
    require(ram[watched[i]] == data,
            "MOV captures the original source before overlapping byte/word writes and preserves "
            "neighboring memory");
  }
}

void data_domain(ObservedCore &core, const std::span<Byte> ram, Point point) {
  const unsigned limit = point.word ? word_mask : byte_mask;
  constexpr std::array<unsigned, 4> flags{0, byte_mask, 0xa5, 0x5a};
  for (unsigned data = 0; data <= limit; ++data) {
    point.data = data;
    if (point.word) {
      for (const auto ccr : flags) {
        point.before.ccr = static_cast<Byte>(ccr);
        one(core, ram, point);
      }
    } else {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        point.before.ccr = static_cast<Byte>(ccr);
        one(core, ram, point);
      }
    }
  }
}

void value_domains() {
  std::array<Byte, address_bytes> ram{};
  ObservedCore core(Memory::unified(ram));
  const auto start = checked;
  for (const bool word : {false, true}) {
    for (const auto source : {Operand{}, source_extended, x}) {
      for (const auto destination : {destination_extended, y}) {
        data_domain(core,
                    ram,
                    {.word = word,
                     .source = source,
                     .destination = destination,
                     .before = seed,
                     .data = 0});
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
              core.statistics().native_instructions == checked - start,
          "All six byte/word MOV combinations use direct native RAM and preserve every CCR bit");
}

void indexed_pairs() {
  std::array<Byte, address_bytes> ram{};
  ObservedCore core(Memory::unified(ram));
  const auto forms = indexes();
  constexpr unsigned count = 236;
  constexpr unsigned data = 0xa55a;
  require(forms.size() == count,
          "Independent MOV addressing model enumerates 236 single-postbyte forms");
  for (const auto source : forms) {
    for (const auto destination : forms) {
      for (const bool word : {false, true}) {
        for (const auto before : seeds) {
          one(core,
              ram,
              {.word = word,
               .source = {.mode = Mode::indexed, .index = source},
               .destination = {.mode = Mode::indexed, .index = destination},
               .before = before,
               .data = word ? data : data & byte_mask});
        }
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "All MOV indexed pairs include shared-base auto updates, accumulator offsets and next-PC "
          "wrapping without helpers");
}

void single_indexed() {
  std::array<Byte, address_bytes> ram{};
  ObservedCore core(Memory::unified(ram));
  constexpr unsigned data = 0x1234;
  for (const auto index : indexes()) {
    for (const bool word : {false, true}) {
      for (const auto before : seeds) {
        const Point point{.word = word,
                          .source = {.mode = Mode::indexed, .index = index},
                          .destination = destination_extended,
                          .before = before,
                          .data = word ? data : data & byte_mask};
        one(core, ram, point);
        for (const auto source : {Operand{}, source_extended}) {
          one(core,
              ram,
              {.word = word,
               .source = source,
               .destination = {.mode = Mode::indexed, .index = index},
               .before = before,
               .data = point.data});
        }
      }
    }
  }
}

void partial_ram() {
  std::array<Byte, address_bytes> ram{};
  constexpr std::uint16_t first = 0x2000;
  constexpr unsigned size = 0x100;
  constexpr std::uint16_t last_word = first + size - 2U;
  ObservedCore core(Memory::window({.first = first, .bytes = std::span(ram).subspan(first, size)}));
  auto before = seed;
  before.x = first;
  before.y = last_word;
  const Operand source{.mode = Mode::extended, .address = first};
  const Operand destination{.mode = Mode::extended, .address = last_word};
  constexpr std::array<unsigned, 8> values{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xa55a, 0xffff};
  for (const bool word : {false, true}) {
    for (const auto from : {Operand{}, source, x}) {
      for (const auto to : {destination, y}) {
        for (const auto value : values) {
          one(core,
              ram,
              {.word = word,
               .source = from,
               .destination = to,
               .before = before,
               .data = word ? value : value & byte_mask});
        }
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Partial-window MOV reads and writes known cells natively without bus callbacks");
}

void block_copy() {
  std::array<Byte, address_bytes> ram{};
  constexpr std::uint16_t source = 0x2000;
  constexpr std::uint16_t destination = 0x3000;
  constexpr std::uint16_t entry = 0x1000;
  constexpr unsigned count = 17;
  constexpr unsigned initial_d = 0x8000 | count;
  constexpr unsigned pattern = 0x1357;
  constexpr unsigned stride = 0x311;
  for (unsigned i = 0; i < count; ++i) {
    const auto data = pattern ^ (i * stride);
    ram[source + (i * 2U)] = static_cast<Byte>(data >> byte_bits);
    ram[source + (i * 2U) + 1U] = static_cast<Byte>(data);
  }
  ObservedCore core(Memory::unified(ram));
  auto initial = seed;
  initial.pc = entry;
  initial.y = source;
  initial.sp = destination;
  initial.d = initial_d;
  core.set_registers(initial);
  constexpr std::array<Byte, 4> copy{0x18, 2, 0x71, 0xb1};
  constexpr std::array<Byte, 3> repeat{4, 0x31, 0xf9};
  for (unsigned i = 0; i < count; ++i) {
    const auto copied = core.run_instruction(copy);
    auto wanted = initial;
    wanted.y = static_cast<std::uint16_t>(source + ((i + 1U) * 2U));
    wanted.sp = static_cast<std::uint16_t>(destination + ((i + 1U) * 2U));
    wanted.d = static_cast<std::uint16_t>(initial_d - i);
    wanted.pc = entry + copy.size();
    require(copied.retired == 1 && copied.fault == Fault::none && core.registers() == wanted,
            "Native MOVW copies the actual next word and advances Y/SP without changing flags or "
            "counter");
    const auto repeated = core.run_instruction(repeat);
    --wanted.d;
    wanted.pc =
        static_cast<std::uint16_t>(entry + (i + 1U == count ? copy.size() + repeat.size() : 0));
    require(repeated.retired == 1 && repeated.fault == Fault::none && core.registers() == wanted,
            "Native DBNE uses the live counter and returns to MOVW until the exact authored copy "
            "count completes");
    checked += 2;
  }
  for (unsigned i = 0; i < count * 2U; ++i) {
    require(
        ram[source + i] == ram[destination + i],
        "Authored block copy retains source bytes and produces the complete expected destination");
  }
  require(ram[destination - 1U] == 0 && ram[destination + (count * 2U)] == 0 &&
              core.statistics().compiled_entries == 2 &&
              core.statistics().native_instructions == std::uint64_t{count} * 2U &&
              core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Two cached native entries complete the exact block without external access or adjacent "
          "writes");
}

void refusals() {
  ObservedCore core;
  std::array<bool, byte_mask + 1U> valid{};
  for (const auto index : indexes()) { valid[index.postbyte] = true; }
  constexpr Byte prefix = 0x18;
  constexpr Byte indexed_pair = 2;
  for (unsigned postbyte = 0; postbyte <= byte_mask; ++postbyte) {
    if (valid[postbyte]) { continue; }
    for (const bool destination : {false, true}) {
      const std::array code{prefix,
                            indexed_pair,
                            destination ? Byte{0} : static_cast<Byte>(postbyte),
                            destination ? static_cast<Byte>(postbyte) : Byte{0}};
      core.set_registers(seed);
      const auto result = core.run_instruction(code);
      require(result.fault == Fault::unsupported_instruction && result.retired == 0 &&
                  core.registers() == seed,
              "Extended and indirect MOV indexed encodings refuse before any actual data access");
    }
  }
  constexpr std::array<Byte, 7> code{prefix, 4, 0, 0, 0, 0, 0};
  for (const unsigned size : {1U, 2U, 3U, 4U, 5U, 7U}) {
    core.set_registers(seed);
    const auto result = core.run_instruction(std::span(code).first(size));
    require(result.fault == (size > maximum_capture_bytes ? Fault::invalid_input
                                                          : Fault::unsupported_instruction) &&
                result.retired == 0 && core.registers() == seed,
            "Truncated and padded MOV captures refuse before retirement");
  }
  require(core.statistics().compiled_entries == 0 && core.statistics().read_callbacks == 0 &&
              core.statistics().write_callbacks == 0,
          "Unsupported MOV operands and malformed captures never compile or touch the bus");
}
}

int main() {
  try {
    value_domains();
    indexed_pairs();
    single_indexed();
    partial_ram();
    block_copy();
    external_cases();
    refusals();
    std::cout << "Verified " << checked
              << " native MOV cases, byte/word domains, all indexed pairs, sequential shared-base "
                 "updates, overlap, unchanged CCR and precise bus faults.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
