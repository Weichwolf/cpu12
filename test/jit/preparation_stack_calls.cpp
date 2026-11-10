// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"
#include "preparation_indexed.hpp"
#include "preparation_stack.hpp"
#include "stack_model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpu12::test::prepared_stack {
namespace {
constexpr Byte jsr = 0x15, jmp = 5;
constexpr unsigned stride = 8;
constexpr std::size_t outer_offset = 0x100, inner_offset = 0x110, nested_bytes = 0x114;
constexpr std::array<Byte, 8> flags{0, 1, 2, 4, 8, 0x10, 0x40, 0xff};

Program indexed_program(const std::span<const prepared_indexed::Form> forms) {
  Program result;
  result.code.resize(forms.size() * stride * 2, padding);
  for (const auto opcode : {jsr, jmp}) {
    for (std::size_t index = 0; index < forms.size(); ++index) {
      const auto offset = (index + (opcode == jmp ? forms.size() : 0)) * stride;
      const auto code = indexed::encode(forms[index].address, opcode);
      std::ranges::copy(code.view(), result.code.begin() + static_cast<std::ptrdiff_t>(offset));
      result.code[offset + code.length] = bra;
      result.code[offset + code.length + 1] = static_cast<Byte>(byte_mask - code.length - 1);
      result.entries.push_back(static_cast<Address>(entry + offset));
      result.entries.push_back(static_cast<Address>(entry + offset + code.length));
    }
  }
  result.nodes = forms.size() * 4;
  result.unresolved = forms.size() * 2;
  return result;
}

void indexed_case(Pair &pair,
                  const prepared_indexed::Form form,
                  const std::size_t slot,
                  Registers before,
                  const bool call) {
  const auto code = indexed::encode(form.address, call ? jsr : jmp);
  before.pc = static_cast<Address>(entry + (slot * stride));
  const auto pointer = indexed::pointer(form.address, before);
  auto wanted = before;
  wanted.pc = form.address.indirect ? stack::word(pair.optimized.bytes, pointer) : pointer;
  auto &base = wanted.*form.address.base;
  if (form.update != 0) { base = static_cast<Address>(static_cast<int>(base) + form.update); }
  if (call) { wanted.sp = static_cast<Address>(static_cast<int>(wanted.sp) - 2); }
  pair.seed(before);
  const auto progress = pair.prepared.progress().estimated_cycles;
  const RunResult expected{.retired = 1, .pc = before.pc, .opcode = call ? jsr : jmp};
  const auto actual = pair.prepared.run(1);
  const State state{.registers = wanted, .failure = {}};
  const auto direct_cycles = form.address.extensions == 2 ? 4U : 3U;
  const auto cycles = (form.address.indirect ? 6U : direct_cycles) + (call ? 1U : 0U);
  if (actual != expected || pair.prepared.state() != state ||
      pair.prepared.progress().estimated_cycles != progress + cycles) {
    throw std::runtime_error("LLVM indexed control post=" + std::to_string(form.address.postbyte) +
                             " PC=" + std::to_string(before.pc) +
                             " SP=" + std::to_string(before.sp));
  }
  require(pair.native.run(1) == actual && pair.native.state() == state &&
              pair.native.progress() == pair.prepared.progress(),
          "Indexed calls/jumps match independently modeled full state and native timing");
  if (call) {
    require(stack::word(pair.optimized.bytes, wanted.sp) ==
                static_cast<Address>(before.pc + code.length),
            "Call stores its actual following PC after SP auto-update, high byte first");
  }
  pair.retained();
}

std::size_t indexed_calls() {
  const auto forms = prepared_indexed::forms();
  Pair pair(indexed_program(forms));
  const auto original = pair.optimized.bytes;
  std::size_t count = 0;
  for (const bool call : {true, false}) {
    for (std::size_t index = 0; index < forms.size(); ++index) {
      for (auto before : stack::seeds) {
        for (const auto ccr : flags) {
          before.ccr = ccr;
          indexed_case(pair, forms[index], index + (call ? 0 : forms.size()), before, call);
          ++count;
        }
      }
    }
  }
  require(pair.optimized.bytes == pair.reference.bytes &&
              std::equal(original.begin() + entry,
                         original.begin() + static_cast<std::ptrdiff_t>(pair.optimized.end),
                         pair.optimized.bytes.begin() + entry),
          "All indexed control cases preserve complete RAM and immutable source bytes");
  return count;
}

std::size_t overlapping_calls() {
  constexpr std::array forms{
      prepared_indexed::Form{.address = {.base = &Registers::sp,
                                         .displacement = -2,
                                         .postbyte = 0xf3,
                                         .extensions = 2,
                                         .indirect = true}},
      prepared_indexed::Form{
          .address = {.base = &Registers::x, .postbyte = 0xe3, .extensions = 2, .indirect = true}}};
  constexpr std::array<Address, 6> stacks{0, 1, 2, 0x7fff, 0xfffe, 0xffff};
  constexpr Address original_target = 0x1234;
  Pair pair(indexed_program(forms));
  std::size_t count = 0;
  for (std::size_t index = 0; index < forms.size(); ++index) {
    for (const auto sp : stacks) {
      for (const auto ccr : flags) {
        auto before = stack::seeds[0];
        before.sp = sp;
        before.x = static_cast<Address>(static_cast<int>(sp) - 2);
        before.ccr = ccr;
        stack::word(pair.optimized.bytes, before.x, original_target);
        stack::word(pair.reference.bytes, before.x, original_target);
        indexed_case(pair, forms[index], index, before, true);
        require(pair.prepared.registers().pc == original_target,
                "Overlapping return stores cannot replace the already resolved indirect target");
        ++count;
      }
    }
  }
  return count;
}

Program nested_program() {
  Program program;
  program.code.resize(nested_bytes, padding);
  constexpr std::array<Byte, 5> main{0x16, 0x41, 0, bra, backedge};
  constexpr std::array<Byte, 8> outer{0x3b, 0xcc, 0x12, 0x34, 7, 10, 0x3a, 0x3d};
  constexpr std::array<Byte, 4> inner{0xce, 0xab, 0xcd, 0x3d};
  std::ranges::copy(main, program.code.begin());
  std::ranges::copy(outer, program.code.begin() + outer_offset);
  std::ranges::copy(inner, program.code.begin() + inner_offset);
  program.entries = {entry};
  program.nodes = 9;
  program.unresolved = 2;
  return program;
}

std::size_t nested_calls() {
  Pair pair(nested_program());
  constexpr Registers seed{
      .pc = entry, .d = 0xbeef, .x = 0, .y = 0x5678, .sp = 0x9000, .ccr = 0xe1};
  constexpr std::array<Registers, 9> states{
      seed,
      Registers{.pc = 0x4100, .d = 0xbeef, .x = 0, .y = seed.y, .sp = 0x8ffe, .ccr = 0xe1},
      Registers{.pc = 0x4101, .d = 0xbeef, .x = 0, .y = seed.y, .sp = 0x8ffc, .ccr = 0xe1},
      Registers{.pc = 0x4104, .d = 0x1234, .x = 0, .y = seed.y, .sp = 0x8ffc, .ccr = 0xe1},
      Registers{.pc = 0x4110, .d = 0x1234, .x = 0, .y = seed.y, .sp = 0x8ffa, .ccr = 0xe1},
      Registers{.pc = 0x4113, .d = 0x1234, .x = 0xabcd, .y = seed.y, .sp = 0x8ffa, .ccr = 0xe9},
      Registers{.pc = 0x4106, .d = 0x1234, .x = 0xabcd, .y = seed.y, .sp = 0x8ffc, .ccr = 0xe9},
      Registers{.pc = 0x4107, .d = 0xbeef, .x = 0xabcd, .y = seed.y, .sp = 0x8ffe, .ccr = 0xe9},
      Registers{.pc = 0x4003, .d = 0xbeef, .x = 0xabcd, .y = seed.y, .sp = 0x9000, .ccr = 0xe9}};
  constexpr std::array<unsigned, 9> cycles{0, 4, 6, 8, 12, 14, 19, 22, 27};
  const auto check = [&](const unsigned grant, const bool timed) {
    pair.seed(seed);
    const auto progress = pair.prepared.progress().estimated_cycles;
    const auto actual =
        timed ? pair.prepared.run_until(progress + grant) : pair.prepared.run(grant);
    const auto native_progress = pair.native.progress().estimated_cycles;
    require((timed ? pair.native.run_until(native_progress + grant) : pair.native.run(grant)) ==
                actual,
            "Nested call/return prefixes and deadlines match ordinary native execution");
    unsigned retired = grant;
    if (timed) {
      retired = 0;
      while (retired < cycles.size() && cycles[retired] < grant) { ++retired; }
      if (retired == cycles.size()) { retired = 8 + (grant - cycles.back() + 2) / 3; }
    }
    const auto expected_cycles =
        retired < cycles.size() ? cycles[retired] : cycles.back() + ((retired - 8) * 3);
    const State expected{.registers = states[std::min(retired, 8U)], .failure = {}};
    require(actual.retired == retired && actual.fault == Fault::none &&
                pair.prepared.state() == expected && pair.native.state() == expected &&
                pair.prepared.progress().estimated_cycles == progress + expected_cycles &&
                pair.prepared.progress() == pair.native.progress() &&
                pair.optimized.bytes == pair.reference.bytes,
            "Nested calls/returns retire the independent complete register/RAM/cycle prefix");
    pair.retained();
  };
  constexpr unsigned last_deadline = 40;
  for (unsigned grant = 0; grant <= last_deadline; ++grant) {
    check(grant, false);
    check(grant, true);
  }
  require(stack::word(pair.optimized.bytes, seed.sp - 2) == entry + 3 &&
              stack::word(pair.optimized.bytes, seed.sp - 4) == seed.d &&
              stack::word(pair.optimized.bytes, seed.sp - 6) == entry + outer_offset + 6,
          "Nested calls preserve actual original return-address and saved-register stores");
  return (std::size_t{last_deadline} + 1) * 2;
}
}

std::size_t calls() {
  return indexed_calls() + overlapping_calls() + nested_calls();
}
}
