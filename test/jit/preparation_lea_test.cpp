// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"
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

namespace cpu12::test::prepared_lea {
namespace {
using indexed::Field;
constexpr Address entry = 0x4000;
constexpr unsigned byte_mask = 0xff, base_shift = 6;
constexpr std::size_t stride = 6;
constexpr std::uint64_t variant_id = 1;
constexpr std::array fields{&Registers::x, &Registers::y, &Registers::sp};
constexpr std::array opcodes{
    isa::Opcode::leax_indexed, isa::Opcode::leay_indexed, isa::Opcode::leas_indexed};
constexpr std::array<Byte, 2> loop{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
constexpr std::array<Registers, 4> seeds{
    Registers{.d = 0, .x = 0, .y = 0xffff, .sp = 1},
    Registers{.d = 0x80ff, .x = 0xffff, .y = 0, .sp = 0x8000},
    Registers{.d = 0x7f80, .x = 0x7fff, .y = 0x8000, .sp = 0xffff},
    Registers{.d = 0xffff, .x = 0x8000, .y = 1, .sp = 0x7fff}};

void require(const bool accepted, const char *message) {
  if (!accepted) { throw std::runtime_error(message); }
}

struct Form {
  indexed::Form address;
  int update = 0;
};

std::vector<Form> forms() {
  std::vector<Form> result;
  for (const auto form : indexed::forms()) {
    if (!form.indirect) { result.push_back({.address = form}); }
  }
  for (std::size_t base = 0; base < fields.size(); ++base) {
    for (int delta = -8; delta <= 8; ++delta) {
      if (delta == 0) { continue; }
      const auto encoded = static_cast<unsigned>(delta > 0 ? delta - 1 : delta + 16);
      for (const bool pre : {false, true}) {
        result.push_back({.address = {.base = fields[base],
                                      .displacement = pre ? delta : 0,
                                      .postbyte = static_cast<Byte>((base << base_shift) | 0x20U |
                                                                    (pre ? 0U : 0x10U) | encoded)},
                          .update = delta});
      }
    }
  }
  return result;
}

struct Site {
  Form form;
  Field target;
  Address pc;
  std::size_t length;
};

struct Source {
  Source() {
    for (const auto form : forms()) {
      for (std::size_t target = 0; target < fields.size(); ++target) {
        const auto encoding = indexed::encode(form.address, static_cast<Byte>(opcodes[target]));
        const auto offset = bytes.size();
        sites.push_back({.form = form,
                         .target = fields[target],
                         .pc = static_cast<Address>(entry + offset),
                         .length = encoding.length});
        bytes.resize(offset + stride, isa::nop[0]);
        std::ranges::copy(encoding.view(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        std::ranges::copy(loop,
                          bytes.begin() + static_cast<std::ptrdiff_t>(offset + encoding.length));
      }
    }
    require(bytes.size() <= address_bytes - entry,
            "The authored LEA inventory must fit its code window");
    for (const auto &site : sites) {
      spans.push_back(
          {.address = site.pc,
           .words = std::span(bytes).subspan(site.pc - entry, site.length + loop.size())});
      entries.push_back(site.pc);
    }
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
  return Core::compile(
      {.variants = variants},
      {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
}

Registers expected(Registers before, const Site &site) {
  const auto value = indexed::pointer(site.form.address, before);
  if (site.form.update != 0) {
    before.*site.form.address.base =
        static_cast<Address>(int{before.*site.form.address.base} + site.form.update);
  }
  before.*site.target = value;
  before.pc = static_cast<Address>(before.pc + site.length);
  return before;
}

template <bool SingleStep> std::size_t matrix(Core &core, const Source &source) {
  const auto compiled = core.statistics().compiled_entries;
  std::size_t count = 0;
  for (const auto &site : source.sites) {
    for (auto before : seeds) {
      before.pc = site.pc;
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        before.ccr = static_cast<Byte>(ccr);
        core.set_registers(before);
        const auto cycles = core.progress().estimated_cycles;
        const auto result = SingleStep ? core.step() : core.run(1);
        require(result.retired == 1 && result.fault == Fault::none && result.pc == before.pc &&
                    core.state() == State{.registers = expected(before, site), .failure = {}} &&
                    core.progress().estimated_cycles == cycles + 2 &&
                    core.statistics().compiled_entries == compiled,
                "Prepared LEA must preserve exact indexed addresses, alias/update ordering, every "
                "CCR and cycles");
        ++count;
      }
    }
  }
  return count;
}

void prefixes(Core &core, const Source &source) {
  const auto compiled = core.statistics().compiled_entries;
  for (const auto &site : source.sites) {
    for (unsigned delta = 0; delta <= 8; ++delta) {
      auto before = seeds[1];
      before.pc = site.pc;
      core.set_registers(before);
      const auto cycles = core.progress().estimated_cycles;
      const auto loops = delta > 2 ? (delta - 2 + 2) / 3 : 0;
      const auto retired = delta == 0 ? 0 : 1 + loops;
      const auto cost = delta == 0 ? 0 : 2 + (loops * 3);
      const auto result = core.run_until(cycles + delta);
      require(result.retired == retired && result.fault == Fault::none &&
                  core.state() == State{.registers = delta == 0 ? before : expected(before, site),
                                        .failure = {}} &&
                  core.progress().estimated_cycles == cycles + cost &&
                  core.statistics().compiled_entries == compiled,
              "Prepared LEA regions preserve independently counted atomic deadline prefixes "
              "without translation");
    }
  }
}

void wrapping() {
  constexpr Address first = 0xffff;
  constexpr std::array<Address, 1> entries{first};
  constexpr std::array regions{RegionLayout{.first = 0, .size = address_bytes, .writable = false}};
  for (std::size_t target = 0; target < fields.size(); ++target) {
    const std::array<Byte, 6> words{
        static_cast<Byte>(opcodes[target]), 0xfa, 0xff, 0xff, loop[0], loop[1]};
    const std::array spans{CodeSpan{.address = first, .words = words}};
    const std::array variants{CodeVariant{.id = variant_id, .code = spans, .entries = entries}};
    const std::array layouts{VariantLayout{.id = variant_id, .memory = regions}};
    const auto image = Core::compile(
        {.variants = variants},
        {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
    std::array<Byte, address_bytes> bytes{};
    for (std::size_t index = 0; index < words.size(); ++index) {
      bytes[static_cast<Address>(first + index)] = words[index];
    }
    Core core(Memory::window({.bytes = bytes, .writable = false}));
    core.install(image, variant_id);
    auto before = seeds[1];
    before.pc = first;
    core.set_registers(before);
    auto wanted = before;
    wanted.pc = 3;
    wanted.*fields[target] = 2;
    require(
        core.run(2).retired == 2 && core.state() == State{.registers = wanted, .failure = {}} &&
            core.progress().estimated_cycles == 5,
        "A wrapped four-byte PC-relative LEA uses its original following PC and exact 16-bit sum");
  }
}
}
}

int main() {
  try {
    using namespace cpu12::test::prepared_lea;
    Source source;
    const auto original = source.bytes;
    const auto image = compile(source);
    cpu12::Core core(
        cpu12::Memory::window({.first = entry, .bytes = source.bytes, .writable = false}));
    core.install(image, variant_id);
    auto count = matrix<true>(core, source);
    prefixes(core, source);
    cpu12::Core prepared(
        cpu12::Memory::window({.first = entry, .bytes = source.bytes, .writable = false}));
    const auto report = prepared.prepare(source.entries);
    require(
        report.failure.fault == cpu12::Fault::none && report.unresolved_entries == 0 &&
            report.prepared_entries == source.sites.size() * 2 && report.native_bytes != 0,
        "Graph preparation must translate every authored LEA and its retained native successor");
    count += matrix<false>(prepared, source);
    prefixes(prepared, source);
    wrapping();
    require(source.bytes == original, "Prepared LEA performs no memory access or source mutation");
    std::cout << "Verified " << count
              << " independently modeled installed LEA states, deadlines and PC wrap.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
