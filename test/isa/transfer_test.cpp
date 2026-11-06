// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
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
constexpr unsigned byte_values = 256;
constexpr unsigned byte_mask = 0xff;
constexpr unsigned word_values = 65536;
constexpr unsigned x_mask = 0x40;
constexpr unsigned exchange_bit = 0x80;
constexpr Byte opcode = 0xb7;
constexpr Registers seed{
    .pc = 0xfffe, .d = 0xa55a, .x = 0x1234, .y = 0xabcd, .sp = 0x8000, .ccr = 0xff};
std::uint64_t checked = 0;

void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

std::uint16_t value(const Registers state, const unsigned selector) {
  switch (selector) {
    case 0: return static_cast<std::uint16_t>(state.d / byte_values);
    case 1: return static_cast<std::uint16_t>(state.d % byte_values);
    case 2: return state.ccr;
    case 4: return state.d;
    case 5: return state.x;
    case 6: return state.y;
    case 7: return state.sp;
    default: throw std::runtime_error("Factory-only selector is not a public register");
  }
}

struct Write {
  unsigned selector, data;
};

void assign(Registers &state, const Write write) {
  const auto data = static_cast<std::uint16_t>(write.data);
  switch (write.selector) {
    case 0:
      state.d = static_cast<std::uint16_t>(((write.data % byte_values) * byte_values) +
                                           (state.d % byte_values));
      break;
    case 1:
      state.d = static_cast<std::uint16_t>(((state.d / byte_values) * byte_values) +
                                           (write.data % byte_values));
      break;
    case 2: state.ccr = static_cast<Byte>(write.data); break;
    case 4: state.d = data; break;
    case 5: state.x = data; break;
    case 6: state.y = data; break;
    case 7: state.sp = data; break;
    default:
      throw std::runtime_error("Factory-only selector cannot receive a fabricated register value");
  }
}

void architectural_write(Registers &state, Write write, const Registers original) {
  if (write.selector == 2 && (original.ccr & x_mask) == 0) { write.data &= byte_mask ^ x_mask; }
  assign(state, write);
}

Registers expected(const Registers before, const unsigned postbyte) {
  const auto source = (postbyte / 16U) % 8U;
  const auto destination = postbyte % 8U;
  const auto first = value(before, source);
  const auto second = value(before, destination);
  auto wanted = before;
  const bool exchange = (postbyte & exchange_bit) != 0;
  unsigned target = first;
  if (exchange) {
    unsigned new_source = second;
    if (source >= 4 && destination < 4 && destination != 0) {
      constexpr unsigned high_byte = 0xff00;
      new_source += high_byte;
    }
    architectural_write(wanted, {.selector = source, .data = new_source}, before);
  } else if (source < 4 && destination >= 4) {
    constexpr unsigned sign = 0x80;
    if (first >= sign) { target += word_values - byte_values; }
  }
  architectural_write(wanted, {.selector = destination, .data = target}, before);
  wanted.pc = static_cast<std::uint16_t>(static_cast<unsigned>(before.pc) + 2U);
  return wanted;
}

void retired(ObservedCore &core,
             const std::span<const Byte> code,
             const Registers before,
             const Registers wanted) {
  core.set_registers(before);
  const auto result = core.run_instruction(code);
  if (result.retired != 1 || result.fault != Fault::none || result.pc != before.pc ||
      result.opcode != code[0] || core.registers() != wanted) {
    const auto actual = core.registers();
    throw std::runtime_error(
        "Transfer postbyte=" + std::to_string(code[1]) + " oldD=" + std::to_string(before.d) +
        " wantedD=" + std::to_string(wanted.d) + " actualD=" + std::to_string(actual.d) +
        " oldCCR=" + std::to_string(before.ccr) + " wantedCCR=" + std::to_string(wanted.ccr) +
        " actualCCR=" + std::to_string(actual.ccr));
  }
  ++checked;
}

struct Domain {
  unsigned postbyte;
  bool destination;
};

void domain(ObservedCore &core, const Domain point) {
  const auto selector = point.destination ? point.postbyte % 8U : (point.postbyte / 16U) % 8U;
  const bool word = selector >= 4;
  const auto values = word ? word_values : byte_values;
  unsigned flags = word ? 2U : byte_values;
  if (selector == 2) { flags = 1; }
  for (unsigned initial = 0; initial < flags; ++initial) {
    for (unsigned data = 0; data < values; ++data) {
      auto before = seed;
      before.ccr = static_cast<Byte>(word ? initial * byte_mask : initial);
      before.pc = static_cast<std::uint16_t>(word_values - (data % 2U) - 1U);
      assign(before, {.selector = selector, .data = data});
      retired(core,
              std::array{opcode, static_cast<Byte>(point.postbyte)},
              before,
              expected(before, point.postbyte));
    }
  }
}

void matrices(ObservedCore &core) {
  constexpr unsigned factory = 3;
  for (unsigned postbyte = 0; postbyte < byte_values; ++postbyte) {
    const auto source = (postbyte / 16U) % 8U;
    const auto destination = postbyte % 8U;
    if (source == factory || destination == factory) { continue; }
    domain(core, {.postbyte = postbyte, .destination = false});
    if (source < 4 && destination >= 4) {
      domain(core, {.postbyte = postbyte, .destination = true});
    }
  }
  constexpr unsigned normal_postbytes = 196;
  require(core.statistics().compiled_entries == normal_postbytes,
          "Each legal raw postbyte, including its ignored bit3, has its own correct cache entry");
}

void literal_overlaps(ObservedCore &core) {
  struct Case {
    Byte postbyte;
    std::uint16_t d;
    Byte ccr;
  };

  constexpr std::array<Case, 10> cases{{{.postbyte = 0x04, .d = 0xff80, .ccr = 0x10},
                                        {.postbyte = 0x14, .d = 0xfffe, .ccr = 0x10},
                                        {.postbyte = 0x84, .d = 0x0080, .ccr = 0x10},
                                        {.postbyte = 0x94, .d = 0x00fe, .ccr = 0x10},
                                        {.postbyte = 0xc0, .d = 0xfe80, .ccr = 0x10},
                                        {.postbyte = 0xc1, .d = 0xfffe, .ccr = 0x10},
                                        {.postbyte = 0xa4, .d = 0x0010, .ccr = 0xbe},
                                        {.postbyte = 0xc2, .d = 0xff10, .ccr = 0xbe},
                                        {.postbyte = 0x42, .d = 0x80fe, .ccr = 0xbe},
                                        {.postbyte = 0x24, .d = 0x0010, .ccr = 0x10}}};
  constexpr Registers before{
      .pc = 0xffff, .d = 0x80fe, .x = 0x1234, .y = 0xabcd, .sp = 0x8000, .ccr = 0x10};
  for (const auto point : cases) {
    auto wanted = before;
    wanted.pc = 1;
    wanted.d = point.d;
    wanted.ccr = point.ccr;
    retired(core, std::array{opcode, point.postbyte}, before, wanted);
  }
}

void flagged_accumulator_transfers(ObservedCore &core) {
  constexpr Byte prefix = 0x18;
  constexpr Byte tab = 0x0e;
  constexpr Byte tba = 0x0f;
  constexpr unsigned sign = 0x80;
  constexpr unsigned nzv = 0x0e;
  for (const auto operation : {tab, tba}) {
    const unsigned source = operation == tab ? 0 : 1;
    const unsigned destination = 1U - source;
    for (unsigned flags = 0; flags < byte_values; ++flags) {
      for (unsigned data = 0; data < byte_values; ++data) {
        auto before = seed;
        before.ccr = static_cast<Byte>(flags);
        assign(before, {.selector = source, .data = data});
        auto wanted = before;
        assign(wanted, {.selector = destination, .data = data});
        wanted.pc = 0;
        wanted.ccr = static_cast<Byte>((flags & (byte_mask ^ nzv)) | (data >= sign ? 8U : 0U) |
                                       (data == 0 ? 4U : 0U));
        retired(core, std::array{prefix, operation}, before, wanted);
      }
    }
  }
}

void refusals(ObservedCore &core) {
  const auto before = core.registers();
  const auto count = core.statistics();
  unsigned factory_cases = 0;
  for (unsigned postbyte = 0; postbyte < byte_values; ++postbyte) {
    if ((postbyte / 16U) % 8U != 3 && postbyte % 8U != 3) { continue; }
    const auto result = core.run_instruction(std::array{opcode, static_cast<Byte>(postbyte)});
    require(result.fault == Fault::unsupported_instruction && result.retired == 0 &&
                result.pc == before.pc && result.opcode == opcode && core.registers() == before,
            "Factory-only temporaries refuse explicitly before architectural or bus effects");
    ++factory_cases;
  }
  constexpr unsigned reserved_postbytes = 60;
  require(factory_cases == reserved_postbytes,
          "Exactly sixty factory-only postbytes remain explicit");
  constexpr std::array<std::array<Byte, 4>, 3> captures{
      {{opcode, 0, 0, 0}, {0x18, 0x0e, 0, 0}, {0x18, 0x0f, 0, 0}}};
  for (const auto input : captures) {
    for (unsigned length = 1; length <= input.size(); ++length) {
      if (length == 2) { continue; }
      require(core.run_instruction(std::span(input).first(length)).fault ==
                      Fault::unsupported_instruction &&
                  core.registers() == before,
              "Valid cached transfers cannot admit incomplete or padded captures");
    }
  }
  require(core.statistics().compiled_entries == count.compiled_entries &&
              core.statistics().native_instructions == count.native_instructions,
          "Refused transfer captures never compile or retire");
}
}

int main() {
  try {
    ObservedCore core;
    matrices(core);
    literal_overlaps(core);
    flagged_accumulator_transfers(core);
    refusals(core);
    require(core.statistics().native_instructions == checked &&
                core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
            "All legal transfer execution is native and touches no external bus or ISA helper");
    std::cout << "Verified " << checked
              << " native transfer/exchange cases, signed and fixed extensions, A/B/D overlaps, "
                 "CCR X restrictions, ignored encoding bits and explicit factory refusals.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
