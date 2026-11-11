// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::test::prepared_transfer {
namespace {
constexpr unsigned byte_values = 256, word_values = 65536, byte_mask = 0xff;
constexpr unsigned irq = 0x10, xirq = 0x40, masks = irq | xirq;
constexpr Byte prefix = 0x18, transfer = 0xb7;
constexpr Address entry = 0x4000;
constexpr std::uint64_t variant_id = 1;
constexpr std::array<Byte, 2> loop{0x20, 0xfe};
constexpr std::array<Address, 8> values{0, 1, 0x7f80, 0x80ff, 0xff00, 0xffff, 0x55aa, 0xaa55};

void require(const bool accepted, const char *message) {
  if (!accepted) { throw std::runtime_error(message); }
}

struct Site {
  Address pc;
  Byte opcode, operand;

  [[nodiscard]] bool flagged() const { return opcode == prefix; }

  [[nodiscard]] unsigned encoding() const {
    if (!flagged()) { return operand; }
    return operand == 0x0e ? 1U : 16U;
  }
};

State expected(const Registers original, const Site site) {
  const auto postbyte = site.encoding();
  const auto from = (postbyte / 16U) % 8U;
  const auto to = postbyte % 8U;
  const std::array<unsigned, 8> snapshot{original.d / byte_values,
                                         original.d % byte_values,
                                         original.ccr,
                                         0,
                                         original.d,
                                         original.x,
                                         original.y,
                                         original.sp};
  auto registers = snapshot;
  const bool exchange = (postbyte & 0x80U) != 0;
  if (exchange) {
    registers[from] = snapshot[to];
    if (from >= 4 && to < 4 && to != 0) { registers[from] += word_values - byte_values; }
  }
  registers[to] = snapshot[from];
  if (!exchange && from < 4 && to >= 4 && snapshot[from] >= byte_values / 2) {
    registers[to] += word_values - byte_values;
  }
  auto wanted = original;
  const auto write = [&](const unsigned target) {
    const auto data = registers[target];
    if (target == 0) {
      wanted.d =
          static_cast<Address>(((data % byte_values) * byte_values) + (wanted.d % byte_values));
    } else if (target == 1) {
      wanted.d =
          static_cast<Address>(((wanted.d / byte_values) * byte_values) + (data % byte_values));
    } else if (target == 2) {
      wanted.ccr =
          static_cast<Byte>(data & ((original.ccr & xirq) != 0 ? byte_mask : byte_mask ^ xirq));
    } else {
      constexpr std::array fields{&Registers::d, &Registers::x, &Registers::y, &Registers::sp};
      wanted.*fields.at(target - 4) = static_cast<Address>(data);
    }
  };
  if (exchange) { write(from); }
  write(to);
  if (site.flagged()) {
    const auto byte = snapshot[from];
    constexpr unsigned nzv = 0x0e;
    wanted.ccr = static_cast<Byte>((original.ccr & (byte_mask ^ nzv)) |
                                   (byte >= byte_values / 2 ? 8U : 0U) | (byte == 0 ? 4U : 0U));
  }
  wanted.pc = static_cast<Address>(original.pc + 2U);
  const auto delay =
      !exchange && from == 0 && to == 2 && (original.ccr & irq) != 0 && (wanted.ccr & irq) == 0;
  return {.registers = wanted, .failure = {}, .irq_delay = static_cast<Byte>(delay)};
}

struct Source {
  Source() {
    for (unsigned postbyte = 0; postbyte < byte_values; ++postbyte) {
      if ((postbyte / 16U) % 8U != 3 && postbyte % 8U != 3) {
        append(transfer, static_cast<Byte>(postbyte));
      }
    }
    append(prefix, 0x0e);
    append(prefix, 0x0f);
    for (const auto site : sites) {
      spans.push_back({.address = site.pc, .words = std::span(bytes).subspan(site.pc - entry, 4)});
      entries.push_back(site.pc);
    }
  }

  void append(const Byte opcode, const Byte operand) {
    sites.push_back(
        {.pc = static_cast<Address>(entry + bytes.size()), .opcode = opcode, .operand = operand});
    bytes.insert(bytes.end(), {opcode, operand, loop[0], loop[1]});
  }

  std::vector<Byte> bytes;
  std::vector<Site> sites;
  std::vector<CodeSpan> spans;
  std::vector<Address> entries;
};

NativeImage compile(const Source &source) {
  const std::array variants{
      CodeVariant{.id = variant_id, .code = source.spans, .entries = source.entries}};
  const std::array regions{
      RegionLayout{.first = entry, .size = source.bytes.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant_id, .memory = regions}};
  return Core::compile({.variants = variants},
                       {.memory = layouts, .interrupts = {}, .transitions = {}});
}

Registers seed(const Site site, const std::size_t index, const Byte ccr) {
  return {.pc = site.pc,
          .d = values[index],
          .x = values[(index + 2) % values.size()],
          .y = values[(index + 4) % values.size()],
          .sp = values[(index + 6) % values.size()],
          .ccr = ccr};
}

template <bool SingleStep> std::size_t matrix(Core &core, const Source &source) {
  const auto compiled = core.statistics().compiled_entries;
  std::size_t checked = 0;
  for (const auto site : source.sites) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      for (unsigned ccr = 0; ccr < byte_values; ++ccr) {
        const auto before = seed(site, index, static_cast<Byte>(ccr));
        core.set_registers(before);
        const auto cycles = core.progress().estimated_cycles;
        const auto result = SingleStep ? core.step() : core.run(1);
        const auto wanted = expected(before, site);
        require(result.retired == 1 && result.fault == Fault::none && result.pc == site.pc &&
                    result.opcode == site.opcode && core.state() == wanted &&
                    core.interrupt_ready(InterruptKind::maskable) ==
                        ((wanted.registers.ccr & irq) == 0 && wanted.irq_delay == 0) &&
                    core.interrupt_ready(InterruptKind::xirq) ==
                        ((wanted.registers.ccr & xirq) == 0) &&
                    core.progress().estimated_cycles == cycles + (site.flagged() ? 2 : 1) &&
                    core.statistics().compiled_entries == compiled,
                "Prepared transfers preserve independent alias/extension/flag/IRQ state and timing "
                "without translation");
        ++checked;
      }
    }
  }
  return checked;
}

void deadline(Core &core, const Site site, const Registers before, const unsigned delta) {
  const auto compiled = core.statistics().compiled_entries;
  core.set_registers(before);
  const auto cycles = core.progress().estimated_cycles;
  const auto result = core.run_until(cycles + delta);
  const auto cost = site.flagged() ? 2U : 1U;
  auto wanted = delta == 0 ? State{.registers = before, .failure = {}} : expected(before, site);
  const bool boundary = ((wanted.registers.ccr ^ before.ccr) & masks) != 0;
  const auto loops = delta > cost && !boundary ? (delta - cost + 2) / 3 : 0;
  const auto count = delta == 0 ? 0 : 1 + loops;
  const auto elapsed = delta == 0 ? 0 : cost + (loops * 3);
  if (loops != 0) { wanted.irq_delay = 0; }
  require(result.retired == count && result.fault == Fault::none && core.state() == wanted &&
              core.progress().estimated_cycles == cycles + elapsed &&
              core.statistics().compiled_entries == compiled,
          "Prepared transfer deadlines stop at exact atomic prefixes and changed interrupt masks");
  if (wanted.irq_delay != 0) {
    wanted.irq_delay = 0;
    const auto resumed = core.run_until(cycles + elapsed + 100);
    require(
        resumed.retired == 1 && resumed.fault == Fault::none && core.state() == wanted &&
            core.progress().estimated_cycles == cycles + elapsed + 3 &&
            core.interrupt_ready(InterruptKind::maskable) &&
            core.statistics().compiled_entries == compiled,
        "Prepared TAP exposes exactly one following instruction before accepting a pending IRQ");
  }
}

void deadlines(Core &core, const Source &source) {
  for (const auto site : source.sites) {
    for (const Byte ccr : {Byte{0}, Byte{irq}, Byte{xirq}, Byte{byte_mask}}) {
      for (unsigned delta = 0; delta <= 12; ++delta) {
        deadline(core, site, seed(site, 3, ccr), delta);
      }
    }
  }
}

struct Bus {
  unsigned calls = 0;

  static bool read(void *owner, Address /*address*/, Byte & /*value*/) noexcept {
    ++static_cast<Bus *>(owner)->calls;
    return false;
  }

  static bool read_word(void *owner, Address /*address*/, Address & /*value*/) noexcept {
    ++static_cast<Bus *>(owner)->calls;
    return false;
  }

  static bool write(void *owner, ByteWrite /*value*/) noexcept {
    ++static_cast<Bus *>(owner)->calls;
    return false;
  }

  static bool write_word(void *owner, WordWrite /*value*/) noexcept {
    ++static_cast<Bus *>(owner)->calls;
    return false;
  }

  Io io() {
    return {.owner = this,
            .read = read,
            .write = write,
            .read_word = read_word,
            .write_word = write_word,
            .fetch = read};
  }
};
}
}

int main() {
  try {
    using namespace cpu12::test::prepared_transfer;
    Source source;
    const auto original = source.bytes;
    const auto image = compile(source);
    Bus bus;
    auto io = bus.io();
    cpu12::Core core(
        cpu12::Memory::window({.first = entry, .bytes = source.bytes, .writable = false}), &io);
    core.install(image, variant_id);
    auto count = matrix<true>(core, source);
    deadlines(core, source);
    cpu12::Core prepared(
        cpu12::Memory::window({.first = entry, .bytes = source.bytes, .writable = false}), &io);
    const auto report = prepared.prepare(source.entries);
    require(
        report.failure.fault == cpu12::Fault::none && report.unresolved_entries == 0 &&
            report.prepared_entries == source.sites.size() * 2 && report.native_bytes != 0,
        "Graph preparation must translate all legal transfer forms and their native continuations");
    count += matrix<false>(prepared, source);
    deadlines(prepared, source);
    require(source.bytes == original && bus.calls == 0,
            "Prepared register transfers perform no memory access or source mutation");
    std::cout << "Verified " << count
              << " independently modeled prepared transfer/exchange states and exact deadlines.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
