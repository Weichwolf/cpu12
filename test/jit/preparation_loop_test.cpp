// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::test::prepared_loop {
namespace {
constexpr Address entry = 0x4000;
constexpr unsigned byte_mask = 0xff, word_mask = 0xffff, byte_values = 256;
constexpr std::size_t stride = 16, instruction_offset = 6;
constexpr std::uint64_t variant_id = 1;
constexpr Byte opcode = static_cast<Byte>(isa::Opcode::dbeq_relative);
constexpr unsigned operation_shift = 6, sign = 0x10, branch_nonzero = 0x20, ignored = 8;
constexpr Byte backward_displacement = 0xf7;
constexpr std::array<Byte, 2> idle{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
constexpr std::array selectors{0U, 1U, 4U, 5U, 6U, 7U};
constexpr std::array fields{&Registers::d, &Registers::x, &Registers::y, &Registers::sp};
constexpr Registers initial{.d = 0xa55a, .x = 0x1234, .y = 0x9876, .sp = 0xffff};

void require(const bool accepted, const char *message) {
  if (!accepted) { throw std::runtime_error(message); }
}

struct Site {
  std::size_t counter;
  int change;
  bool nonzero;
  Address pc, destination;
};

unsigned mask(const Site site) {
  return site.counter < 2 ? byte_mask : word_mask;
}

unsigned value(const Registers state, const Site site) {
  if (site.counter == 0) { return state.d / byte_values; }
  if (site.counter == 1) { return state.d % byte_values; }
  return state.*fields.at(site.counter - 2);
}

Registers assign(Registers state, const Site site, const unsigned data) {
  if (site.counter == 0) {
    state.d = static_cast<Address>((data * byte_values) + (state.d % byte_values));
  } else if (site.counter == 1) {
    state.d = static_cast<Address>(((state.d / byte_values) * byte_values) + data);
  } else {
    state.*fields.at(site.counter - 2) = static_cast<Address>(data);
  }
  return state;
}

Registers expected(const Registers before, const Site site) {
  const auto modulus = mask(site) + 1;
  const auto changed = static_cast<unsigned>(static_cast<int>(value(before, site)) + site.change +
                                             static_cast<int>(modulus)) %
                       modulus;
  auto wanted = assign(before, site, changed);
  wanted.pc =
      (changed != 0) == site.nonzero ? site.destination : static_cast<Address>(site.pc + 3U);
  return wanted;
}

struct Source {
  Source() {
    for (std::size_t counter = 0; counter < selectors.size(); ++counter) {
      for (unsigned operation = 0; operation < 3; ++operation) {
        for (const bool nonzero : {false, true}) {
          for (const bool backward : {false, true}) {
            for (const bool alias : {false, true}) {
              append(counter, operation, nonzero, backward, alias);
            }
          }
        }
      }
    }
    for (const auto site : sites) {
      entries.push_back(site.pc);
      spans.push_back({.address = site.pc, .words = std::span(bytes).subspan(site.pc - entry, 5)});
      spans.push_back({.address = site.destination,
                       .words = std::span(bytes).subspan(site.destination - entry, idle.size())});
    }
  }

  void append(const std::size_t counter,
              const unsigned operation,
              const bool nonzero,
              const bool backward,
              const bool alias) {
    const auto start = bytes.size();
    const auto pc = static_cast<Address>(entry + start + instruction_offset);
    constexpr std::array changes{-1, 0, 1};
    const auto destination = static_cast<Address>(pc + (backward ? -6 : 6));
    sites.push_back({.counter = counter,
                     .change = changes[operation],
                     .nonzero = nonzero,
                     .pc = pc,
                     .destination = destination});
    bytes.resize(start + stride);
    bytes[start + instruction_offset] = opcode;
    bytes[start + instruction_offset + 1] = static_cast<Byte>(
        (operation << operation_shift) | selectors[counter] | (nonzero ? branch_nonzero : 0) |
        (backward ? sign : 0) | (alias ? ignored : 0));
    bytes[start + instruction_offset + 2] = backward ? backward_displacement : Byte{3};
    std::ranges::copy(idle,
                      bytes.begin() + static_cast<std::ptrdiff_t>(start + instruction_offset + 3));
    std::ranges::copy(idle, bytes.begin() + static_cast<std::ptrdiff_t>(destination - entry));
  }

  std::vector<Byte> bytes;
  std::vector<Site> sites;
  std::vector<Address> entries;
  std::vector<CodeSpan> spans;
};

NativeImage compile(const Source &source) {
  const std::array variants{
      CodeVariant{.id = variant_id, .code = source.spans, .entries = source.entries}};
  const std::array regions{
      RegionLayout{.first = entry, .size = source.bytes.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant_id, .memory = regions}};
  return Core::compile({.variants = variants}, {.memory = layouts, .interrupts = {}});
}

template <bool SingleStep> void check(Core &core, const Site site, Registers before) {
  before.pc = site.pc;
  core.set_registers(before);
  const auto cycles = core.progress().estimated_cycles;
  const auto compiled = core.statistics().compiled_entries;
  const auto result = SingleStep ? core.step() : core.run(1);
  require(result.retired == 1 && result.fault == Fault::none && result.pc == site.pc &&
              result.opcode == opcode &&
              core.state() == State{.registers = expected(before, site), .failure = {}} &&
              core.progress().estimated_cycles == cycles + 3 &&
              core.statistics().compiled_entries == compiled,
          "Prepared loops preserve independently modeled counter width, wrap, branch decision, CCR "
          "and cycles");
}

template <bool SingleStep> std::uint64_t matrix(Core &core, const Source &source) {
  std::uint64_t checked = 0;
  for (const auto site : source.sites) {
    for (unsigned data = 0; data <= mask(site); ++data) {
      const auto state = assign(initial, site, data);
      if (site.counter < 2) {
        for (unsigned ccr = 0; ccr < byte_values; ++ccr) {
          auto before = state;
          before.ccr = static_cast<Byte>(ccr);
          check<SingleStep>(core, site, before);
          ++checked;
        }
      } else {
        for (const Byte ccr : {Byte{0}, Byte{byte_mask}}) {
          auto before = state;
          before.ccr = ccr;
          check<SingleStep>(core, site, before);
          ++checked;
        }
      }
    }
  }
  return checked;
}

void deadlines(Core &core, const Source &source) {
  constexpr std::array<unsigned, 6> counters{0, 1, 2, 0x7f, byte_mask, word_mask};
  for (const auto site : source.sites) {
    for (const auto counter : counters) {
      auto before = assign(initial, site, counter & mask(site));
      before.pc = site.pc;
      before.ccr = byte_mask;
      for (unsigned delta = 0; delta <= 12; ++delta) {
        core.set_registers(before);
        const auto cycles = core.progress().estimated_cycles;
        const auto compiled = core.statistics().compiled_entries;
        const auto result = core.run_until(cycles + delta);
        const auto count = (delta + 2) / 3;
        require(result.retired == count && result.fault == Fault::none &&
                    core.state() == State{.registers = delta == 0 ? before : expected(before, site),
                                          .failure = {}} &&
                    core.progress().estimated_cycles == cycles + (std::uint64_t{count} * 3) &&
                    core.statistics().compiled_entries == compiled,
                "Prepared loop regions retain exact counter and branch prefixes at every deadline");
      }
    }
  }
}

void wrapping() {
  constexpr std::array<Address, 2> starts{0xfffe, 0xffff};
  constexpr std::array regions{RegionLayout{.first = 0, .size = address_bytes, .writable = false}};
  const std::array layouts{VariantLayout{.id = variant_id, .memory = regions}};
  for (const auto pc : starts) {
    for (const bool backward : {false, true}) {
      constexpr int minimum_displacement = -256;
      constexpr int maximum_displacement = 255;
      constexpr unsigned increment = 2U << operation_shift;
      const Site site{.counter = 4,
                      .change = 1,
                      .nonzero = true,
                      .pc = pc,
                      .destination = static_cast<Address>(
                          int{pc} + 3 + (backward ? minimum_displacement : maximum_displacement))};
      const std::array<Byte, 3> instruction{opcode,
                                            static_cast<Byte>(increment | branch_nonzero |
                                                              selectors[site.counter] | ignored |
                                                              (backward ? sign : 0)),
                                            backward ? Byte{0} : Byte{byte_mask}};
      const auto next = static_cast<Address>(pc + instruction.size());
      const std::array spans{CodeSpan{.address = pc, .words = instruction},
                             CodeSpan{.address = next, .words = idle},
                             CodeSpan{.address = site.destination, .words = idle}};
      const std::array<Address, 1> entries{pc};
      const std::array variants{CodeVariant{.id = variant_id, .code = spans, .entries = entries}};
      const auto image =
          Core::compile({.variants = variants}, {.memory = layouts, .interrupts = {}});
      std::array<Byte, address_bytes> bytes{};
      for (const auto span : spans) {
        for (std::size_t index = 0; index < span.words.size(); ++index) {
          bytes[static_cast<Address>(span.address + index)] = span.words[index];
        }
      }
      Core core(Memory::window({.bytes = bytes, .writable = false}));
      core.install(image, variant_id);
      for (const unsigned counter : {0U, 1U, word_mask}) {
        auto before = assign(initial, site, counter);
        before.pc = pc;
        before.ccr = byte_mask;
        core.set_registers(before);
        const auto cycles = core.progress().estimated_cycles;
        const auto compiled = core.statistics().compiled_entries;
        require(core.run(2).retired == 2 &&
                    core.state() == State{.registers = expected(before, site), .failure = {}} &&
                    core.progress().estimated_cycles == cycles + 6 &&
                    core.statistics().compiled_entries == compiled,
                "Prepared wrapped loop encodings preserve both nine-bit displacement endpoints and "
                "successors");
      }
    }
  }
}
}
}

int main() {
  try {
    using namespace cpu12::test::prepared_loop;
    Source source;
    const auto original = source.bytes;
    const auto image = compile(source);
    cpu12::Core core(
        cpu12::Memory::window({.first = entry, .bytes = source.bytes, .writable = false}));
    core.install(image, variant_id);
    auto count = matrix<true>(core, source);
    deadlines(core, source);
    cpu12::Core prepared(
        cpu12::Memory::window({.first = entry, .bytes = source.bytes, .writable = false}));
    const auto report = prepared.prepare(source.entries);
    require(report.failure.fault == cpu12::Fault::none && report.unresolved_entries == 0 &&
                report.prepared_entries == source.sites.size() * 3 && report.native_bytes != 0,
            "Graph preparation must translate every authored loop and both native successors");
    count += matrix<false>(prepared, source);
    deadlines(prepared, source);
    wrapping();
    require(source.bytes == original,
            "Prepared register loops perform no memory access or source mutation");
    std::cout << "Verified " << count
              << " independently modeled prepared loop states and exact deadlines.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
