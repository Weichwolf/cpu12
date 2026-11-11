// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "multiply_divide_model.hpp"
#include "preparation_math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace cpu12::test::prepared_math {
namespace {
void prefix(Core &core, const math::Input point, const unsigned delta) {
  core.set_registers(point.before);
  const auto progress = core.progress().estimated_cycles;
  const auto cost = cycles(point.operation);
  const auto loops = delta > cost ? (delta - cost + 2) / 3 : 0;
  const auto count = delta == 0 ? 0 : 1 + loops;
  const auto elapsed = delta == 0 ? 0 : cost + (loops * 3);
  const auto execution = core.run_until(progress + delta);
  const auto wanted = delta == 0 ? point.before : expected(point).registers;
  const auto last = loops != 0 ? wanted.pc : point.before.pc;
  auto opcode = loops != 0 ? idle[0] : math::encode(point.operation).bytes[0];
  if (delta == 0) { opcode = 0; }
  require(execution.retired == count && execution.fault == Fault::none && execution.pc == last &&
              execution.opcode == opcode &&
              core.state() == State{.registers = wanted, .failure = {}} &&
              core.progress().estimated_cycles == progress + elapsed,
          "Prepared math preserves exact atomic deadline prefixes, including zero divisors and "
          "overflowing signed quotients");
}

struct ChainPoint {
  Registers registers;
  unsigned budget;
};

void chain_prefix(Core &core, const ChainPoint point) {
  auto wanted = point.registers;
  std::uint64_t cycles_elapsed = 0;
  Address last = wanted.pc;
  Byte opcode = 0;
  for (unsigned step = 0; step < point.budget; ++step) {
    last = wanted.pc;
    if (step < operations.size()) {
      const auto operation = operations[step];
      wanted = expected({.operation = operation, .before = wanted}).registers;
      opcode = math::encode(operation).bytes[0];
      cycles_elapsed += cycles(operation);
    } else {
      opcode = idle[0];
      cycles_elapsed += 3;
    }
  }
  core.set_registers(point.registers);
  const auto progress = core.progress().estimated_cycles;
  const auto execution = core.run(point.budget);
  require(execution.retired == point.budget && execution.fault == Fault::none &&
              execution.pc == last && execution.opcode == opcode &&
              core.state() == State{.registers = wanted, .failure = {}} &&
              core.progress().estimated_cycles == progress + cycles_elapsed,
          "A contiguous prepared math chain preserves dependent operands, every CCR bit and exact "
          "native prefixes");
}

void chain_domain(Core &core) {
  constexpr std::array<Address, 8> edges{0, 1, 2, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff};
  for (const auto d : edges) {
    for (const auto x : edges) {
      for (const auto y : edges) {
        for (const auto ccr : flags) {
          const Registers state{.pc = first, .d = d, .x = x, .y = y, .sp = word_mask, .ccr = ccr};
          for (unsigned budget = 0; budget <= 32; ++budget) {
            chain_prefix(core, {.registers = state, .budget = budget});
          }
        }
      }
    }
  }
}
}

void prefixes(Core &core, math::Input point) {
  for (const auto data : anchors) {
    point.before.d = data;
    point.before.x = static_cast<Address>(word_mask - data);
    constexpr Address sign = 0x8000;
    point.before.y = static_cast<Address>(data ^ sign);
    for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
      point.before.ccr = static_cast<Byte>(ccr);
      for (unsigned delta = 0; delta <= 15; ++delta) { prefix(core, point, delta); }
    }
  }
}

void wrapped(const Operation operation) {
  constexpr std::array<Address, 2> pcs{0xfffe, 0xffff};
  const auto instruction = math::encode(operation);
  std::vector<Byte> code(instruction.view().begin(), instruction.view().end());
  code.insert(code.end(), idle.begin(), idle.end());
  for (const auto pc : pcs) {
    const std::array spans{CodeSpan{.address = pc, .words = code}};
    const std::array entries{pc};
    const std::array variants{CodeVariant{.id = variant, .code = spans, .entries = entries}};
    const std::array regions{RegionLayout{.first = 0, .size = address_bytes, .writable = false}};
    const std::array layouts{VariantLayout{.id = variant, .memory = regions}};
    const auto image = Core::compile({.variants = variants},
                                     {.memory = layouts, .interrupts = {}, .transitions = {}});
    std::array<Byte, address_bytes> bytes{};
    for (std::size_t index = 0; index < code.size(); ++index) {
      bytes[static_cast<Address>(pc + index)] = code[index];
    }
    Core core(Memory::window({.first = 0, .bytes = bytes, .writable = false}));
    core.install(image, variant);
    Count count;
    for (const auto data : anchors) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        auto state = math::seed;
        state.pc = pc;
        state.d = data;
        state.x = static_cast<Address>(word_mask - data);
        state.y = data;
        state.ccr = static_cast<Byte>(ccr);
        check(core, {.operation = operation, .before = state}, count);
      }
    }
  }
}

void chain() {
  const Source source(true);
  const auto image = compile(source);
  auto code = source.bytes;
  auto second = code;
  Core installed(Memory::window({.first = first, .bytes = code, .writable = false}));
  Core graph(Memory::window({.first = first, .bytes = second, .writable = false}));
  installed.install(image, variant);
  const std::array entries{first};
  const auto report = graph.prepare(entries);
  require(report.failure.fault == Fault::none && report.unresolved_entries == 0 &&
              report.prepared_entries == operations.size() + 1 && report.native_bytes != 0,
          "The complete contiguous arithmetic chain is prepared before execution");
  for (Core *core : {&installed, &graph}) {
    const auto compiled = core->statistics().compiled_entries;
    chain_domain(*core);
    require(core->statistics().compiled_entries == compiled,
            "Math-chain execution cannot compile native code");
  }
  require(code == source.bytes && second == source.bytes,
          "Math chains retain all declared executable bytes");
  std::cout << "135168 independent contiguous native math prefixes passed.\n";
}
}
