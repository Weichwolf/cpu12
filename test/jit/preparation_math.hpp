// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "divide_model.hpp"
#include "isa.hpp"
#include "multiply_divide_model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>
#include <vector>

namespace cpu12::test::prepared_math {
using math::Operation;
using math::require;
constexpr Address first = 0x4000;
constexpr unsigned byte_mask = 0xff, word_mask = 0xffff, word_bits = 16;
constexpr std::uint64_t variant = 1;
constexpr std::array<Byte, 2> idle{static_cast<Byte>(isa::Opcode::bra_relative), 0xfe};
constexpr std::array operations{Operation::mul,
                                Operation::emul,
                                Operation::emuls,
                                Operation::idiv,
                                Operation::idivs,
                                Operation::ediv,
                                Operation::edivs,
                                Operation::fdiv};
constexpr std::array<std::string_view, operations.size()> names{
    "mul", "emul", "emuls", "idiv", "idivs", "ediv", "edivs", "fdiv"};
constexpr std::array<Address, 14> anchors{
    0, 1, 2, 3, 7, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff};
constexpr std::array<Byte, 4> flags{0, 0xff, 0xa5, 0x5a};
constexpr std::array<Address, 4> words{0, 0x7fff, 0x8000, 0xffff};

inline unsigned cycles(const Operation operation) {
  if (operation == Operation::mul) { return 1; }
  if (operation == Operation::emul || operation == Operation::emuls) { return 3; }
  return operation == Operation::ediv ? 11 : 12;
}

inline math::Expected expected(const math::Input point) {
  if (point.operation == Operation::mul || point.operation == Operation::emul ||
      point.operation == Operation::emuls) {
    return math::multiply(point);
  }
  return math::divide_model::expected(point);
}

struct Source {
  explicit Source(const bool chain = false) {
    for (const auto operation : operations) {
      const auto code = math::encode(operation);
      const auto at = static_cast<Address>(first + bytes.size());
      entries.push_back(at);
      std::ranges::copy(code.view(), std::back_inserter(bytes));
      if (!chain) {
        std::ranges::copy(idle, std::back_inserter(bytes));
        constexpr std::size_t stride = 8;
        bytes.resize(entries.size() * stride);
      }
    }
    if (chain) {
      std::ranges::copy(idle, std::back_inserter(bytes));
      spans.push_back({.address = first, .words = bytes});
    } else {
      for (std::size_t index = 0; index < operations.size(); ++index) {
        spans.push_back(
            {.address = entries[index],
             .words = std::span(bytes).subspan(
                 entries[index] - first, math::encode(operations[index]).length + idle.size())});
      }
    }
  }

  std::vector<Byte> bytes;
  std::vector<Address> entries;
  std::vector<CodeSpan> spans;
};

inline NativeImage compile(const Source &source) {
  const std::array regions{
      RegionLayout{.first = first, .size = source.bytes.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = variant, .memory = regions}};
  const std::array variants{
      CodeVariant{.id = variant, .code = source.spans, .entries = source.entries}};
  return Core::compile(
      {.variants = variants},
      {.memory = layouts, .interrupts = {}, .transitions = {}, .callbacks = {}, .bindings = {}});
}

struct Count {
  std::uint64_t states = 0, policy = 0;
};

inline void check(Core &core, const math::Input point, Count &count) {
  const auto wanted = expected(point);
  core.set_registers(point.before);
  const auto before = core.progress().estimated_cycles;
  const auto execution = core.run(1);
  require(execution.retired == 1 && execution.fault == Fault::none &&
              execution.pc == point.before.pc &&
              execution.opcode == math::encode(point.operation).bytes[0] &&
              core.state() == State{.registers = wanted.registers, .failure = {}} &&
              core.progress().estimated_cycles == before + cycles(point.operation),
          "Prepared math preserves independently specified registers, defined flags, core error "
          "policy and exact timing without host traps");
  ++count.states;
  if (wanted.undefined_d || wanted.defined_ccr != byte_mask) { ++count.policy; }
}

void domains(Core &core, math::Input point, Count &count);
void prefixes(Core &core, math::Input point);
void wrapped(Operation operation);
void chain();
}
