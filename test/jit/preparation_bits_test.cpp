// SPDX-License-Identifier: GPL-3.0-only
#include "bits_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "preparation_bits.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <vector>

namespace cpu12::test::prepared_bits {
namespace {
struct Ram {
  explicit Ram(const Source &source) : code(source.bytes) {}

  std::vector<Byte> code;
  Byte value = 0, direct = 0;
  std::array<cpu12::Ram, 3> regions;

  Memory memory() {
    regions = {cpu12::Ram{.first = first, .bytes = code, .writable = false},
               cpu12::Ram{.first = operand, .bytes = std::span(&value, 1)},
               cpu12::Ram{.first = direct_operand, .bytes = std::span(&direct, 1)}};
    return Memory::mapped(regions);
  }

  Byte &data(const Site &site) { return site.form == Form::direct ? direct : value; }
};

void check(Core &core, Ram &ram, const Site &site, const Byte data, const Byte ccr) {
  const auto point = site.point({.data = data, .ccr = ccr});
  core.set_registers(point.before);
  ram.data(site) = data;
  const auto compiled = core.statistics().compiled_entries;
  const auto progress = core.progress().estimated_cycles;
  const auto execution = core.run(1);
  const auto wanted = bits::expected(point, site.code.length);
  const auto value = site.operation.branch
                         ? data
                         : bits::modified(site.operation, {.data = data, .mask = site.mask});
  require(
      execution.retired == 1 && execution.fault == Fault::none && execution.pc == site.pc &&
          execution.opcode == site.code.bytes[0] &&
          core.state() == State{.registers = wanted, .failure = {}} && ram.data(site) == value &&
          core.progress().estimated_cycles == progress + site.cycles() &&
          core.statistics().compiled_entries == compiled,
      "Prepared bits preserve independent masks/data/flags/index/PC/timing without translation");
}

std::uint64_t matrix(Core &core, Ram &ram, const Source &source) {
  std::uint64_t count = 0;
  constexpr std::array<Byte, 4> flags{0, 1, 0xa5, 0xff};
  for (const auto &site : source.sites) {
    if (site.form == Form::extended) {
      for (unsigned data = 0; data <= byte_mask; ++data) {
        for (const auto ccr : flags) {
          check(core, ram, site, static_cast<Byte>(data), ccr);
          ++count;
        }
      }
    } else {
      for (const auto data : edges) {
        for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
          check(core, ram, site, data, static_cast<Byte>(ccr));
          ++count;
        }
      }
    }
  }
  return count;
}

void deadlines(Core &core, Ram &ram, const Source &source) {
  for (const auto &site : source.sites) {
    for (const Byte data : {Byte{0}, Byte{0xff}}) {
      const auto point = site.point({.data = data, .ccr = byte_mask});
      for (unsigned delta = 0; delta <= 15; ++delta) {
        ram.data(site) = data;
        core.set_registers(point.before);
        const auto progress = core.progress().estimated_cycles;
        const auto compiled = core.statistics().compiled_entries;
        const auto cost = site.cycles();
        const auto loops = delta > cost ? (delta - cost + 2) / 3 : 0;
        const auto count = delta == 0 ? 0 : 1 + loops;
        const auto elapsed = delta == 0 ? 0 : cost + (loops * 3);
        const auto execution = core.run_until(progress + delta);
        require(execution.retired == count && execution.fault == Fault::none &&
                    core.state() ==
                        State{.registers = delta == 0 ? point.before
                                                      : bits::expected(point, site.code.length),
                              .failure = {}} &&
                    core.progress().estimated_cycles == progress + elapsed &&
                    core.statistics().compiled_entries == compiled,
                "Prepared bit decisions retain exact deadlines through both native successors");
      }
    }
  }
}

void wrapped_site(const Site &site, const int displacement) {
  auto instruction = site.code;
  instruction.bytes[instruction.length - 1] = static_cast<Byte>(displacement);
  std::vector<Byte> stream(instruction.view().begin(), instruction.view().end());
  stream.insert(stream.end(), idle.begin(), idle.end());
  const auto next = static_cast<Address>(site.pc + instruction.length);
  const auto target = static_cast<Address>(static_cast<int>(next) + displacement);
  const std::array spans{CodeSpan{.address = site.pc, .words = stream},
                         CodeSpan{.address = target, .words = idle}};
  const std::array entries{site.pc};
  const std::array variants{CodeVariant{.id = variant, .code = spans, .entries = entries}};
  const std::array regions{RegionLayout{.first = 0, .size = address_bytes, .writable = false}};
  const std::array layouts{VariantLayout{.id = variant, .memory = regions}};
  const auto image = Core::compile({.variants = variants}, {.memory = layouts, .interrupts = {}});
  std::array<Byte, address_bytes> bytes{};
  for (std::size_t offset = 0; offset < stream.size(); ++offset) {
    bytes[static_cast<Address>(site.pc + offset)] = stream[offset];
  }
  bytes[target] = idle[0];
  bytes[static_cast<Address>(target + 1)] = idle[1];
  Core core(Memory::window({.first = 0, .bytes = bytes, .writable = false}));
  core.install(image, variant);
  for (const Byte data : {Byte{0}, Byte{byte_mask}}) {
    auto model = site.point({.data = data, .ccr = byte_mask});
    model.displacement = displacement;
    bytes[site.address()] = data;
    core.set_registers(model.before);
    const auto before = bytes;
    const auto progress = core.progress().estimated_cycles;
    const auto compiled = core.statistics().compiled_entries;
    const auto execution = core.run_until(progress + site.cycles() + 3);
    require(execution.retired == 2 && execution.fault == Fault::none &&
                core.state() ==
                    State{.registers = bits::expected(model, instruction.length), .failure = {}} &&
                core.progress().estimated_cycles == progress + site.cycles() + 3 &&
                core.statistics().compiled_entries == compiled && before == bytes,
            "Prepared bit branches preserve wrapping encodings, signed byte displacement endpoints "
            "and both successor prefixes");
  }
}

void wrapping() {
  constexpr std::array<Address, 2> addresses{0xfffe, 0xffff};
  constexpr std::array displacements{-128, 127};
  for (const auto operation : bits::operations) {
    if (!operation.branch) { continue; }
    for (const auto form : forms) {
      for (const auto pc : addresses) {
        for (const auto displacement : displacements) {
          wrapped_site(encode(operation, form, byte_mask, pc), displacement);
        }
      }
    }
  }
}

}
}

int main() {
  try {
    using namespace cpu12::test::prepared_bits;
    const Source source(true);
    const auto image = compile(source, false);
    Ram ram(source);
    cpu12::Core installed(ram.memory());
    installed.install(image, variant);
    auto count = matrix(installed, ram, source);
    deadlines(installed, ram, source);
    Ram second(source);
    cpu12::Core graph(second.memory());
    const auto prepared = graph.prepare(source.entries);
    const auto branches = static_cast<std::uint64_t>(std::ranges::count_if(
        source.sites, [](const auto &site) { return site.operation.branch; }));
    require(prepared.failure.fault == cpu12::Fault::none && prepared.unresolved_entries == 0 &&
                prepared.prepared_entries == source.sites.size() * 2 + branches &&
                prepared.native_bytes != 0,
            "Every bit encoding and both successors are prepared before playback");
    count += matrix(graph, second, source);
    deadlines(graph, second, source);
    require(ram.code == source.bytes && second.code == source.bytes,
            "Bit operations preserve executable source bytes");
    bus_cases();
    wrapping();
    std::cout << "Verified " << count
              << " independently modeled prepared bit states and prefix deadlines.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
