// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "preparation_memory_unary.hpp"
#include "unary_model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

namespace cpu12::test::prepared_memory_unary {
namespace {
struct Ram {
  explicit Ram(const Source &source) : code_bytes(source.bytes.size()) {
    std::ranges::copy(source.bytes, bytes.begin() + entry);
    bytes[pointer] = static_cast<Byte>(operand >> byte_bits);
    bytes[0] = static_cast<Byte>(operand);
    known.fill(1);
  }

  std::array<Byte, address_bytes> bytes{}, known{};
  std::array<cpu12::Ram, 4> grants;
  std::size_t code_bytes;
  std::uint64_t pointer_reads = 0;

  static bool read_word(void *owner, const Address at, Address &value) noexcept {
    auto &ram = *static_cast<Ram *>(owner);
    ++ram.pointer_reads;
    if (at != pointer) { return false; }
    value = static_cast<Address>((unsigned{ram.bytes[pointer]} << byte_bits) | ram.bytes[0]);
    return ram.known[pointer] != 0 && ram.known[0] != 0;
  }

  Io io() { return {.owner = this, .read_word = read_word}; }

  Memory memory(const Backing backing) {
    const auto flags = [&](const Address at) {
      return backing == Backing::tracked ? std::span(known).subspan(at, 1) : std::span<Byte>{};
    };
    grants = {cpu12::Ram{.first = entry,
                         .bytes = std::span(bytes).subspan(entry, code_bytes),
                         .writable = false},
              cpu12::Ram{.first = operand,
                         .bytes = std::span(bytes).subspan(operand, 1),
                         .known = flags(operand)},
              cpu12::Ram{.first = pointer,
                         .bytes = std::span(bytes).subspan(pointer, 1),
                         .known = flags(pointer)},
              cpu12::Ram{.first = 0, .bytes = std::span(bytes).first(1), .known = flags(0)}};
    return Memory::mapped(grants);
  }
};

template <bool SingleStep>
void check(Core &core, Ram &ram, const Site site, const Byte data, const Byte ccr) {
  const auto before = seed(site, ccr);
  const auto result = unary::expected(
      {.function = site.operation.function, .data = data, .ccr = ccr, .word = false});
  ram.bytes[operand] = data;
  core.set_registers(before);
  const auto progress = core.progress().estimated_cycles;
  const auto compiled = core.statistics().compiled_entries;
  const auto pointers = ram.pointer_reads;
  const auto execution = SingleStep ? core.step() : core.run(1);
  const bool matches =
      execution.retired == 1 && execution.fault == Fault::none && execution.pc == site.pc &&
      execution.opcode == site.code[0] &&
      core.state() == State{.registers = expected(before, site, data), .failure = {}} &&
      core.progress().estimated_cycles == progress + cycles(site) &&
      ram.bytes[operand] == (site.operation.write ? result.data : data) &&
      core.statistics().compiled_entries == compiled &&
      ram.pointer_reads == pointers + (site.form == Form::indirect ? 1U : 0U);
  if (!matches) {
    std::cerr << "opcode=" << unsigned{site.code[0]} << " form=" << static_cast<unsigned>(site.form)
              << " input=" << unsigned{data} << " oldCCR=" << unsigned{ccr}
              << " retired=" << execution.retired
              << " fault=" << static_cast<unsigned>(execution.fault)
              << " actualCCR=" << unsigned{core.registers().ccr} << " wantedCCR=" << result.flags
              << " cycles=" << core.progress().estimated_cycles - progress
              << " wantedCycles=" << cycles(site) << " output=" << unsigned{ram.bytes[operand]}
              << " wantedOutput=" << result.data << " actualX=" << core.registers().x
              << " wantedX=" << expected(before, site, data).x << " actualY=" << core.registers().y
              << " wantedY=" << expected(before, site, data).y
              << " actualSP=" << core.registers().sp
              << " wantedSP=" << expected(before, site, data).sp << '\n';
  }
  require(matches,
          "Prepared memory unary preserves independent data/CCR/index/timing semantics without "
          "translation");
}

template <bool SingleStep> std::uint64_t matrix(Core &core, Ram &ram, const Source &source) {
  std::uint64_t count = 0;
  for (const auto site : source.sites) {
    if (site.form == Form::extended) {
      for (unsigned data = 0; data <= byte_mask; ++data) {
        for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
          check<SingleStep>(core, ram, site, static_cast<Byte>(data), static_cast<Byte>(ccr));
          ++count;
        }
      }
    } else {
      for (const auto data : edges) {
        for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
          check<SingleStep>(core, ram, site, data, static_cast<Byte>(ccr));
          ++count;
        }
      }
    }
  }
  return count;
}

void deadlines(Core &core, Ram &ram, const Source &source) {
  for (const auto site : source.sites) {
    constexpr Byte data = 0x80;
    const auto before = seed(site, byte_mask);
    for (unsigned delta = 0; delta <= 12; ++delta) {
      ram.bytes[operand] = data;
      core.set_registers(before);
      const auto progress = core.progress().estimated_cycles;
      const auto cost = cycles(site);
      const auto loops = delta > cost ? (delta - cost + 2) / 3 : 0;
      const auto count = delta == 0 ? 0 : 1 + loops;
      const auto elapsed = delta == 0 ? 0 : cost + (loops * 3);
      const auto compiled = core.statistics().compiled_entries;
      const auto execution = core.run_until(progress + delta);
      require(execution.retired == count && execution.fault == Fault::none &&
                  core.state() ==
                      State{.registers = delta == 0 ? before : expected(before, site, data),
                            .failure = {}} &&
                  core.progress().estimated_cycles == progress + elapsed &&
                  core.statistics().compiled_entries == compiled,
              "Prepared memory unary retains exact atomic deadlines across both successors");
    }
  }
}

void tracked(const Source &source) {
  const auto image = compile(source, Backing::tracked);
  Ram ram(source);
  auto io = ram.io();
  Core core(ram.memory(Backing::tracked), &io);
  core.install(image, variant_id);
  for (const auto site : source.sites) {
    for (const auto data : edges) { check<true>(core, ram, site, data, byte_mask); }
    ram.known[operand] = 0;
    core.set_registers(seed(site, byte_mask));
    const auto before = core.state();
    const auto progress = core.progress();
    const auto compiled = core.statistics().compiled_entries;
    const auto execution = core.step();
    if (site.operation.read) {
      auto wanted = before;
      wanted.failure = {.fault = Fault::memory_read,
                        .pc = site.pc,
                        .opcode = site.code[0],
                        .address = operand,
                        .access_size = 1,
                        .read_mask = byte_mask};
      require(execution.retired == 0 && core.state() == wanted && core.progress() == progress &&
                  ram.known[operand] == 0,
              "An unknown tracked source faults before flags, index or data publication");
    } else {
      require(execution.retired == 1 && execution.fault == Fault::none && ram.bytes[operand] == 0 &&
                  ram.known[operand] == 1,
              "CLR writes a known zero without reading unknown tracked data");
    }
    require(core.statistics().compiled_entries == compiled,
            "Tracked memory unary cannot trigger first-use translation");
    ram.known[operand] = 1;
  }
}
}
}

int main() {
  try {
    using namespace cpu12::test::prepared_memory_unary;
    const Source source;
    const auto image = compile(source, Backing::ram);
    Ram ram(source);
    auto io = ram.io();
    cpu12::Core core(ram.memory(Backing::ram), &io);
    core.install(image, variant_id);
    auto count = matrix<true>(core, ram, source);
    deadlines(core, ram, source);
    Ram second(source);
    auto second_io = second.io();
    cpu12::Core prepared(second.memory(Backing::ram), &second_io);
    const auto report = prepared.prepare(source.entries);
    require(report.failure.fault == cpu12::Fault::none && report.unresolved_entries == 0 &&
                report.prepared_entries == source.sites.size() * 2 && report.native_bytes != 0,
            "Graph preparation lowers every memory unary operation and retained successor");
    count += matrix<false>(prepared, second, source);
    deadlines(prepared, second, source);
    tracked(source);
    bus_cases(source);
    require(std::ranges::equal(source.bytes,
                               std::span(ram.bytes).subspan(entry, source.bytes.size())) &&
                std::ranges::equal(source.bytes,
                                   std::span(second.bytes).subspan(entry, source.bytes.size())),
            "Memory unary modifies data only, retaining all declared executable bytes");
    std::cout
        << "Verified " << count
        << " independently modeled prepared memory-unary states, tracked data and bus phases.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
