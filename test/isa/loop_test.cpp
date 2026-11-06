// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
using Byte = std::uint8_t;
constexpr unsigned byte_mask = 0xff, word_mask = 0xffff, byte_bits = 8;
constexpr Registers seed{.pc = 0xfffe, .d = 0xa55a, .x = 0x8000, .y = 0x7fff, .sp = 1, .ccr = 0xff};
std::uint64_t checked = 0;

enum class Counter : Byte { a, b, d, x, y, sp };

struct Target {
  Counter reg;
  Byte selector;
  unsigned mask;
};

constexpr std::array<Target, 6> targets{{{.reg = Counter::a, .selector = 0, .mask = byte_mask},
                                         {.reg = Counter::b, .selector = 1, .mask = byte_mask},
                                         {.reg = Counter::d, .selector = 4, .mask = word_mask},
                                         {.reg = Counter::x, .selector = 5, .mask = word_mask},
                                         {.reg = Counter::y, .selector = 6, .mask = word_mask},
                                         {.reg = Counter::sp, .selector = 7, .mask = word_mask}}};

struct Operation {
  Byte postbyte;
  int change;
  bool nonzero;
};

constexpr std::array<Operation, 6> operations{{{.postbyte = 0x00, .change = -1, .nonzero = false},
                                               {.postbyte = 0x20, .change = -1, .nonzero = true},
                                               {.postbyte = 0x40, .change = 0, .nonzero = false},
                                               {.postbyte = 0x60, .change = 0, .nonzero = true},
                                               {.postbyte = 0x80, .change = 1, .nonzero = false},
                                               {.postbyte = 0xa0, .change = 1, .nonzero = true}}};

void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

Registers counter(Registers state, const Counter target, const unsigned value) {
  switch (target) {
    case Counter::a:
      state.d = static_cast<std::uint16_t>((state.d & byte_mask) | (value << byte_bits));
      break;
    case Counter::b:
      state.d = static_cast<std::uint16_t>((state.d & (word_mask ^ byte_mask)) | value);
      break;
    case Counter::d: state.d = static_cast<std::uint16_t>(value); break;
    case Counter::x: state.x = static_cast<std::uint16_t>(value); break;
    case Counter::y: state.y = static_cast<std::uint16_t>(value); break;
    case Counter::sp: state.sp = static_cast<std::uint16_t>(value); break;
    default: throw std::runtime_error("Unknown independent loop-counter expectation");
  }
  return state;
}

struct Case {
  Target target;
  Operation operation;
  int displacement;
  bool alias;
};

std::array<Byte, 3> encode(const Case point) {
  constexpr Byte sign = 0x10;
  constexpr Byte ignored = 8;
  return {4,
          static_cast<Byte>(point.operation.postbyte + point.target.selector +
                            (point.displacement < 0 ? sign : 0) + (point.alias ? ignored : 0)),
          static_cast<Byte>(point.displacement)};
}

struct State {
  Registers registers;
  unsigned value;
};

void one(ObservedCore &core, const Case point, const State state) {
  const auto before = counter(state.registers, point.target.reg, state.value);
  const auto value = static_cast<unsigned>(static_cast<int>(state.value) + point.operation.change +
                                           static_cast<int>(point.target.mask + 1U)) &
                     point.target.mask;
  const bool taken = (value != 0) == point.operation.nonzero;
  auto wanted = counter(before, point.target.reg, value);
  wanted.pc = static_cast<std::uint16_t>(static_cast<int>(before.pc) + 3 +
                                         (taken ? point.displacement : 0));
  core.set_registers(before);
  const auto result = core.run_instruction(encode(point));
  require(result.fault == Fault::none && result.retired == 1 && result.pc == before.pc &&
              result.opcode == 4 && core.registers() == wanted,
          "Native loop must match independent actual-width counter, signed relative PC and all CCR "
          "bits");
  ++checked;
}

void value_domain(ObservedCore &core, const Case point) {
  constexpr std::array<unsigned, 4> flags{0, byte_mask, 0xa5, 0x5a};
  for (unsigned value = 0; value <= point.target.mask; ++value) {
    if (point.target.mask == byte_mask) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        auto state = seed;
        state.ccr = static_cast<Byte>(ccr);
        one(core, point, {.registers = state, .value = value});
      }
    } else {
      for (const auto ccr : flags) {
        auto state = seed;
        state.ccr = static_cast<Byte>(ccr);
        one(core, point, {.registers = state, .value = value});
      }
    }
  }
}

void counter_domains() {
  ObservedCore core;
  for (const auto target : targets) {
    for (const auto operation : operations) {
      for (const bool alias : {false, true}) {
        value_domain(
            core, {.target = target, .operation = operation, .displacement = -3, .alias = alias});
      }
    }
  }
  require(
      core.statistics().compiled_entries == targets.size() * operations.size() * 2 &&
          core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
          core.statistics().native_instructions == checked,
      "Every counter/operation and ignored-bit encoding caches separately with no memory helpers");
}

void displacement_domain(ObservedCore &core, const Case point) {
  constexpr std::array<unsigned, 6> values{0, 1, 2, 0x7f, 0x80, word_mask};
  constexpr std::array<std::uint16_t, 5> pcs{0, 1, 0x7fff, 0xfffd, 0xffff};
  for (const auto pc : pcs) {
    for (const auto value : values) {
      auto state = seed;
      state.pc = pc;
      one(core, point, {.registers = state, .value = value & point.target.mask});
    }
  }
}

void all_displacements() {
  ObservedCore core;
  constexpr int first = -256;
  constexpr int last = 255;
  for (const auto target : targets) {
    for (const auto operation : operations) {
      for (int displacement = first; displacement <= last; ++displacement) {
        for (const bool alias : {false, true}) {
          displacement_domain(core,
                              {.target = target,
                               .operation = operation,
                               .displacement = displacement,
                               .alias = alias});
        }
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Signed loop target calculations never read target memory or guest flags");
}

void all_flags() {
  ObservedCore core;
  constexpr int positive_limit = 255;
  for (const auto target : targets) {
    for (const auto operation : operations) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        auto state = seed;
        state.ccr = static_cast<Byte>(ccr);
        for (const unsigned value : {0U, 1U, word_mask}) {
          one(core,
              {.target = target,
               .operation = operation,
               .displacement = positive_limit,
               .alias = false},
              {.registers = state, .value = value & target.mask});
        }
      }
    }
  }
}

void postbytes_and_refusals() {
  ObservedCore core;
  for (unsigned postbyte = 0; postbyte <= byte_mask; ++postbyte) {
    const unsigned operation = postbyte / 64U;
    const unsigned selector = postbyte % 8U;
    const bool valid = operation < 3 && selector != 2 && selector != 3;
    const std::array<Byte, 3> code{4, static_cast<Byte>(postbyte), 0};
    core.set_registers(seed);
    const auto result = core.run_instruction(code);
    require(valid ? result.fault == Fault::none && result.retired == 1
                  : result.fault == Fault::unsupported_instruction && result.retired == 0 &&
                        core.registers() == seed,
            "All 144 documented loop postbytes execute and 112 undocumented encodings refuse");
  }
  constexpr unsigned accepted_postbytes = 144;
  require(core.statistics().compiled_entries == accepted_postbytes &&
              core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Undocumented loop captures never compile, retire or cause real bus accesses");
  constexpr std::array<Byte, 4> code{4, 0, 0, 0};
  for (const unsigned size : {0U, 1U, 2U, 4U}) {
    core.set_registers(seed);
    const auto result = core.run_instruction(std::span(code).first(size));
    require(result.retired == 0 && core.registers() == seed &&
                result.fault == (size == 0 ? Fault::invalid_input : Fault::unsupported_instruction),
            "Truncated and padded loop captures refuse without CPU mutation");
  }
}

void repeated_loop() {
  ObservedCore core;
  auto before = seed;
  constexpr std::uint16_t loop_pc = 0x1000;
  before.pc = loop_pc;
  constexpr std::uint16_t initial_d = 0x035a;
  before.d = initial_d;
  core.set_registers(before);
  constexpr std::array<Byte, 3> dbne{4, 0x30, 0xfd}; // DBNE A,-3.
  for (unsigned step = 1; step <= 3; ++step) {
    const auto result = core.run_instruction(dbne);
    auto wanted = before;
    wanted.d = static_cast<std::uint16_t>(((3U - step) << byte_bits) | (before.d & byte_mask));
    wanted.pc = static_cast<std::uint16_t>(loop_pc + (step == 3 ? 3 : 0));
    require(
        result.retired == 1 && result.fault == Fault::none && core.registers() == wanted,
        "Warm DBNE repeats at its actual current PC and falls through exactly when A reaches zero");
    ++checked;
  }
  require(core.statistics().compiled_entries == 1 && core.statistics().native_instructions == 3,
          "Repeated loop executes the same cached native entry with changing live counter state");
}
}

int main() {
  try {
    counter_domains();
    all_displacements();
    all_flags();
    postbytes_and_refusals();
    repeated_loop();
    std::cout
        << "Verified " << checked
        << " native loop cases, all counter values, signed9 offsets, unchanged CCR, ignored-bit "
           "aliases and explicit undocumented refusals.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
