// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "multiply_divide_model.hpp"
#include "observed_core.hpp"
#include "resident_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

using cpu12::test::ObservedCore;

namespace {
using namespace cpu12;
namespace math = cpu12::test::math;
using cpu12::test::resident::Device;
using cpu12::test::resident::entry;
using cpu12::test::resident::require;
constexpr unsigned mask = 0xffff, sign = 0x8000, byte_mask = 255;
constexpr unsigned negative = 8, zero = 4, overflow = 2, carry = 1;
constexpr unsigned retained = 0xf0;
constexpr Address marker_x = 123, stack = 0x9000;
constexpr std::array<Address, 7> values{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xffff};
constexpr std::array<Byte, 2> minimum_signed{0x80, 0};
unsigned checked = 0;

unsigned nz(const unsigned value) {
  return (value == 0 ? zero : 0U) | (value >= sign ? negative : 0U);
}

Registers compare_y(Registers state) {
  const auto difference = (unsigned{state.y} + mask + 1U - sign) & mask;
  // Subtracting -32768 from a nonnegative signed operand overflows.
  state.ccr = static_cast<Byte>((state.ccr & retained) | nz(difference) |
                                (state.y < sign ? carry | overflow : 0U));
  state.pc += 3;
  return state;
}

Registers shift(Registers state) {
  const auto old = state.d;
  state.d = static_cast<Address>((unsigned{old} * 2U) & mask);
  const bool old_negative = old >= sign;
  const bool next_negative = state.d >= sign;
  state.ccr = static_cast<Byte>((state.ccr & retained) | nz(state.d) | (old_negative ? carry : 0U) |
                                (old_negative != next_negative ? overflow : 0U));
  ++state.pc;
  return state;
}

enum class Form : Byte { plain, shifted, prior_compare };

template <isa::Opcode opcode, Form form> void products() {
  constexpr auto program = [] {
    if constexpr (form == Form::shifted) {
      return isa::program(isa::opcode<opcode>(), isa::asld, isa::nop);
    } else if constexpr (form == Form::prior_compare) {
      return isa::program(isa::opcode<isa::Opcode::cpy_immediate>(),
                          minimum_signed,
                          isa::opcode<opcode>(),
                          isa::nop);
    } else {
      return isa::program(isa::opcode<opcode>(), isa::nop);
    }
  }();
  constexpr auto operation =
      opcode == isa::Opcode::emuls ? math::Operation::emuls : math::Operation::emul;
  constexpr unsigned instructions = form == Form::plain ? 2 : 3;
  Device device(program);
  const auto before_ram = device.ram;
  for (unsigned flags = 0; flags <= byte_mask; ++flags) {
    for (const auto d : values) {
      for (const auto y : values) {
        std::array<Registers, instructions + 1> trace{};
        trace[0] = {.pc = entry,
                    .d = d,
                    .x = marker_x,
                    .y = y,
                    .sp = stack,
                    .ccr = static_cast<Byte>(flags)};
        unsigned stage = 0;
        if constexpr (form == Form::prior_compare) { trace[++stage] = compare_y(trace[0]); }
        const auto result = math::multiply({.operation = operation, .before = trace[stage]});
        trace[++stage] = result.registers;
        if constexpr (form == Form::shifted) {
          const auto next = shift(trace[stage]);
          trace[++stage] = next;
        }
        auto final = trace[stage];
        ++final.pc; // Original NOP, ending the immutable ROM fragment.
        trace[++stage] = final;
        for (unsigned grant = 1; grant <= instructions; ++grant) {
          device.core.set_registers(trace[0]);
          const auto actual = device.core.run(grant);
          const auto at = trace[grant - 1].pc;
          require(actual.retired == grant && actual.fault == Fault::none && actual.pc == at &&
                      actual.opcode == program[at - entry] &&
                      device.core.registers() == trace[grant] && device.ram == before_ram &&
                      actual.state == ExecutionState::running,
                  "Resident multiply/shift differs from independent full-state/flag expectations");
          ++checked;
        }
      }
    }
  }
}

template <isa::Opcode branch> bool taken(const Byte flags) {
  if constexpr (branch == isa::Opcode::bcc_relative) { return (flags & carry) == 0; }
  if constexpr (branch == isa::Opcode::bcs_relative) { return (flags & carry) != 0; }
  if constexpr (branch == isa::Opcode::beq_relative) { return (flags & zero) != 0; }
  return (flags & zero) == 0;
}

template <isa::Opcode branch> void branches() {
  constexpr std::array<Byte, 1> displacement{1};
  constexpr auto program = isa::program(isa::emuls, isa::opcode<branch>(), displacement, isa::nop);
  Device device(program);
  const auto before = device.ram;
  for (unsigned flags = 0; flags <= byte_mask; ++flags) {
    for (const auto d : values) {
      for (const auto y : values) {
        const Registers initial{.pc = entry,
                                .d = d,
                                .x = marker_x,
                                .y = y,
                                .sp = stack,
                                .ccr = static_cast<Byte>(flags)};
        auto expected =
            math::multiply({.operation = math::Operation::emuls, .before = initial}).registers;
        const bool take = taken<branch>(expected.ccr);
        expected.pc = static_cast<Address>(expected.pc + 2U + (take ? 1U : 0U));
        device.core.set_registers(initial);
        const auto actual = device.core.run(2);
        require(actual.retired == 2 && actual.fault == Fault::none &&
                    actual.pc == entry + isa::emuls.size() &&
                    actual.opcode == isa::opcode<branch>()[0] &&
                    device.core.registers() == expected && device.ram == before,
                "A multiply branch must use full-product Z and product-bit-15 C");
        ++checked;
      }
    }
  }
}

void standalone_shift() {
  constexpr auto program = isa::program(isa::asld, isa::nop);
  Device device(program);
  const auto before = device.ram;
  for (unsigned flags = 0; flags <= byte_mask; ++flags) {
    for (const auto value : values) {
      const Registers initial{.pc = entry,
                              .d = value,
                              .x = marker_x,
                              .y = sign,
                              .sp = stack,
                              .ccr = static_cast<Byte>(flags)};
      for (const unsigned grant : {1U, 2U}) {
        auto expected = shift(initial);
        if (grant == 2) { ++expected.pc; }
        device.core.set_registers(initial);
        const auto actual = device.core.run(grant);
        require(actual.fault == Fault::none && actual.retired == grant &&
                    device.core.registers() == expected && device.ram == before,
                "Standalone ASLD must update D and preserve the independent X/Y values");
        ++checked;
      }
    }
  }
}

void nonlinear_loop() {
  constexpr std::array<Byte, 1> backward{0xfd};
  constexpr auto program =
      isa::program(isa::asld, isa::opcode<isa::Opcode::bne_relative>(), backward, isa::nop);
  Device device(program);
  const auto before = device.ram;
  constexpr std::array<unsigned, 6> grants{1, 2, 3, 7, 16, 33};
  for (unsigned flags = 0; flags <= byte_mask; ++flags) {
    for (const auto value : values) {
      const Registers initial{.pc = entry,
                              .d = value,
                              .x = marker_x,
                              .y = 0x4567,
                              .sp = stack,
                              .ccr = static_cast<Byte>(flags)};
      for (const auto grant : grants) {
        auto expected = initial;
        unsigned retired = 0;
        Address last = entry;
        while (retired < grant && expected.pc < entry + program.size()) {
          last = expected.pc;
          if (last == entry) {
            expected = shift(expected);
          } else if (last == entry + 1) {
            expected.pc = expected.d == 0 ? entry + 3 : entry;
          } else {
            ++expected.pc;
          }
          ++retired;
        }
        device.core.set_registers(initial);
        const auto actual = device.core.run(retired);
        require(actual.retired == retired && actual.fault == Fault::none && actual.pc == last &&
                    device.core.registers() == expected && device.ram == before,
                "ASLD/BNE cannot be treated as a linear X/Y counted loop");
        ++checked;
      }
    }
  }
}

template <isa::Opcode opcode> void divide_boundary() {
  constexpr auto program = isa::program(isa::opcode<opcode>(), isa::idiv, isa::nop);
  constexpr auto operation =
      opcode == isa::Opcode::emuls ? math::Operation::emuls : math::Operation::emul;
  constexpr std::array<Address, 4> divisors{1, 3, 7, marker_x};
  Device device(program);
  const auto before = device.ram;
  for (const Byte flags : {Byte{0}, Byte{0xff}}) {
    for (const auto left : values) {
      for (const auto right : values) {
        for (const auto divisor : divisors) {
          const Registers initial{
              .pc = entry, .d = left, .x = divisor, .y = right, .sp = stack, .ccr = flags};
          const auto product =
              math::multiply({.operation = operation, .before = initial}).registers;
          auto quotient = product;
          quotient.x = static_cast<Address>(product.d / divisor);
          quotient.d = static_cast<Address>(product.d % divisor);
          constexpr Byte divide_retained = 0xf8;
          quotient.ccr =
              static_cast<Byte>((product.ccr & divide_retained) | (quotient.x == 0 ? 4U : 0U));
          quotient.pc += isa::idiv.size();
          auto final = quotient;
          ++final.pc;
          const std::array trace{initial, product, quotient, final};
          for (unsigned grant = 1; grant < trace.size(); ++grant) {
            device.check(initial, grant, trace[grant]);
            require(device.ram == before, "Resident/divide boundaries preserve all guest RAM");
            ++checked;
          }
        }
      }
    }
  }
}

struct CachedInput {
  Address left, right, count;
  Byte flags;
};

template <isa::Opcode opcode> Registers cached_result(const CachedInput point, const Address end) {
  auto left = point.left;
  std::uint64_t product = 0;
  for (unsigned index = 0; index < point.count; ++index) {
    if constexpr (opcode == isa::Opcode::emuls) {
      product =
          static_cast<std::uint64_t>(math::signed_word(left) * math::signed_word(point.right)) &
          math::long_mask;
    } else {
      product = std::uint64_t{left} * point.right;
    }
    left = static_cast<Address>((product + 1) & mask);
  }
  return {.pc = end,
          .d = left,
          .x = 0,
          .y = static_cast<Address>(product >> math::word_bits),
          .sp = stack,
          .ccr = static_cast<Byte>((point.flags & retained) | zero |
                                   (left >= sign ? negative : 0U) | (left == 0 ? carry : 0U))};
}

template <isa::Opcode opcode> void cached_products() {
  constexpr Address cached_data = 0x4000;
  constexpr Address stale_d = 0xa55a;
  constexpr Address stale_y = 0x9876;
  constexpr auto head = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                                     std::array<Byte, 2>{0x40, 0},
                                     isa::opcode<isa::Opcode::ldy_extended>(),
                                     std::array<Byte, 2>{0x40, 2},
                                     isa::opcode<opcode>(),
                                     isa::opcode<isa::Opcode::cpd_immediate>(),
                                     std::array<Byte, 2>{0, 0},
                                     isa::opcode<isa::Opcode::bcs_relative>(),
                                     std::array<Byte, 1>{9});
  constexpr auto tail = isa::program(isa::opcode<isa::Opcode::addd_immediate>(),
                                     std::array<Byte, 2>{0, 1},
                                     isa::opcode<isa::Opcode::std_extended>(),
                                     std::array<Byte, 2>{0x40, 0},
                                     isa::dex,
                                     isa::opcode<isa::Opcode::bne_relative>());
  constexpr std::array<Byte, 1> back{
      static_cast<Byte>(byte_mask + 1U - (head.size() + tail.size() + 1))};
  constexpr auto program = isa::program(head, tail, back, isa::nop);
  const auto put = [](auto &ram, const Address at, const Address value) {
    ram[at] = static_cast<Byte>(value >> 8U);
    ram[at + 1] = static_cast<Byte>(value & byte_mask);
  };
  Device device(program);
  for (const Byte flags : {Byte{0}, Byte{byte_mask}}) {
    for (const auto left : values) {
      for (const auto right : values) {
        for (const Address count : {Address{1}, Address{3}}) {
          put(device.ram, cached_data, left);
          put(device.ram, cached_data + 2, right);
          const auto expected =
              cached_result<opcode>({.left = left, .right = right, .count = count, .flags = flags},
                                    static_cast<Address>(entry + program.size()));
          auto expected_ram = device.ram;
          put(expected_ram, cached_data, expected.d);
          const Registers initial{
              .pc = entry, .d = stale_d, .x = count, .y = stale_y, .sp = stack, .ccr = flags};
          device.check(initial, (9U * count) + 1U, expected);
          require(device.ram == expected_ram,
                  "Cached signed/unsigned products retain all final RAM and register effects");
          ++checked;
        }
      }
    }
  }
}

enum class Square : Byte { same, overlap, live_flags };
constexpr Address square_data = 0x4000, square_alias = 0x5000;
constexpr Address square_d = 0x1357, square_y = 0x2468;
constexpr Byte square_guard = 0xa5;
using SquareBytes = std::array<Byte, 8>;

template <std::size_t size> struct SquareDevice {
  std::array<Byte, size> rom;
  SquareBytes bytes{};
  std::array<Ram, 3> regions;
  ObservedCore core;

  explicit SquareDevice(const std::array<Byte, size> program)
      : rom(program),
        regions{Ram{.first = entry, .bytes = rom, .writable = false},
                Ram{.first = square_data, .bytes = bytes},
                Ram{.first = square_alias, .bytes = bytes}},
        core(Memory::mapped(regions)) {}
};

struct SquarePoint {
  Address value;
  Byte flags;
};

Registers transfer(Registers state, const Address value) {
  constexpr unsigned keep = 0xf1;
  state.ccr = static_cast<Byte>((state.ccr & keep) | nz(value));
  state.pc += 3;
  return state;
}

template <isa::Opcode opcode, Square form> struct SquareCase {
  static constexpr std::array<Byte, 2> source{0x40, 0}, high{0x40, 4}, low{0x40, 6};
  static constexpr std::array<Byte, 2> alias{0x50, form == Square::overlap ? 1 : 0};
  static constexpr auto program = [] {
    constexpr auto inputs = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                                         source,
                                         isa::opcode<isa::Opcode::ldy_extended>(),
                                         alias,
                                         isa::opcode<opcode>());
    if constexpr (form == Square::live_flags) {
      return isa::program(inputs, isa::nop, isa::nop, isa::nop);
    } else {
      return isa::program(inputs,
                          isa::opcode<isa::Opcode::sty_extended>(),
                          high,
                          isa::opcode<isa::Opcode::std_extended>(),
                          low,
                          isa::nop);
    }
  }();
  std::array<Registers, 7> trace{};
  std::array<SquareBytes, 7> images{};

  explicit SquareCase(const SquarePoint point) {
    images[0].fill(square_guard);
    images[0][0] = static_cast<Byte>(point.value >> 8U);
    images[0][1] = static_cast<Byte>(point.value & byte_mask);
    images.fill(images[0]);
    trace[0] = {
        .pc = entry, .d = square_d, .x = marker_x, .y = square_y, .sp = stack, .ccr = point.flags};
    trace[1] = transfer(trace[0], point.value);
    trace[1].d = point.value;
    const auto y = form == Square::overlap
                       ? static_cast<Address>((unsigned{images[0][1]} << 8U) | images[0][2])
                       : point.value;
    trace[2] = transfer(trace[1], y);
    trace[2].y = y;
    constexpr auto operation =
        opcode == isa::Opcode::emuls ? math::Operation::emuls : math::Operation::emul;
    trace[3] = math::multiply({.operation = operation, .before = trace[2]}).registers;
    for (unsigned index = 3; index < 6; ++index) {
      trace[index + 1] = trace[index];
      images[index + 1] = images[index];
      if (form == Square::live_flags || index == 5) {
        ++trace[index + 1].pc;
      } else {
        const auto value = index == 3 ? trace[index].y : trace[index].d;
        const unsigned offset = index == 3 ? 4 : 6;
        trace[index + 1] = transfer(trace[index], value);
        images[index + 1][offset] = static_cast<Byte>(value >> 8U);
        images[index + 1][offset + 1] = static_cast<Byte>(value & byte_mask);
      }
    }
  }
};

template <isa::Opcode opcode, Square form>
void square_case(SquareDevice<SquareCase<opcode, form>::program.size()> &device,
                 const SquarePoint point,
                 const bool full_only) {
  const SquareCase<opcode, form> expected(point);
  for (unsigned grant = full_only ? 6U : 1U; grant <= 6; ++grant) {
    device.bytes = expected.images[0];
    device.core.set_registers(expected.trace[0]);
    const auto actual = device.core.run(grant);
    const auto at = expected.trace[grant - 1].pc;
    const State state{.registers = expected.trace[grant], .failure = {}};
    require(actual ==
                    RunResult{.retired = grant, .pc = at, .opcode = expected.program[at - entry]} &&
                device.core.state() == state && device.bytes == expected.images[grant] &&
                device.rom == expected.program,
            "Native squares lost operand identity, live flags, exact grants or original stores");
    ++checked;
  }
}

template <isa::Opcode opcode, Square form> void squares() {
  SquareDevice device(SquareCase<opcode, form>::program);
  for (unsigned flags = 0; flags <= byte_mask; ++flags) {
    for (const auto value : values) {
      square_case<opcode, form>(device, {.value = value, .flags = static_cast<Byte>(flags)}, false);
    }
  }
  if constexpr (form == Square::same) {
    for (unsigned value = 0; value <= mask; ++value) {
      square_case<opcode, form>(
          device, {.value = static_cast<Address>(value), .flags = byte_mask}, true);
    }
  }
}
}

int main() {
  try {
    cached_products<isa::Opcode::emul>();
    cached_products<isa::Opcode::emuls>();
    divide_boundary<isa::Opcode::emul>();
    divide_boundary<isa::Opcode::emuls>();
    products<isa::Opcode::emul, Form::plain>();
    products<isa::Opcode::emuls, Form::plain>();
    products<isa::Opcode::emul, Form::shifted>();
    products<isa::Opcode::emuls, Form::shifted>();
    products<isa::Opcode::emul, Form::prior_compare>();
    products<isa::Opcode::emuls, Form::prior_compare>();
    branches<isa::Opcode::bcc_relative>();
    branches<isa::Opcode::bcs_relative>();
    branches<isa::Opcode::beq_relative>();
    branches<isa::Opcode::bne_relative>();
    standalone_shift();
    nonlinear_loop();
    squares<isa::Opcode::emul, Square::same>();
    squares<isa::Opcode::emuls, Square::same>();
    squares<isa::Opcode::emul, Square::overlap>();
    squares<isa::Opcode::emuls, Square::overlap>();
    squares<isa::Opcode::emul, Square::live_flags>();
    squares<isa::Opcode::emuls, Square::live_flags>();
    std::cout << checked << " independent resident multiply/shift/branch boundaries passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
