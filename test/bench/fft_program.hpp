// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>

namespace cpu12::test::fft {
inline constexpr unsigned points = 16, stages = 4, butterfly_count = points * stages / 2;
inline constexpr unsigned byte_bits = 8, byte_mask = 255, word_mask = 65535, sign = 32768;
inline constexpr Address entry = 0x0200, data = 0x4000, twiddles = 0x4100;
inline constexpr Address product = 0x4200, real = product + 4, imaginary = product + 6;
inline constexpr Address term = product + 8, previous = product + 10, stack = 0x9000;
inline constexpr unsigned scratch_bytes = 12, instructions_per_butterfly = 46;
inline constexpr unsigned retired = (butterfly_count * instructions_per_butterfly) + 1;
inline constexpr unsigned retained_flags = 0x70, frame_bytes = 9;
using Image = std::array<Byte, address_bytes>;
using Samples = std::array<Address, std::size_t{points} * 2>;

consteval auto address(const Address at) {
  return std::array<Byte, 2>{static_cast<Byte>(at >> byte_bits), static_cast<Byte>(at & byte_mask)};
}

consteval auto multiply(const Address left, const Address right) {
  return isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                      address(left),
                      isa::opcode<isa::Opcode::ldy_extended>(),
                      address(right),
                      isa::emuls,
                      isa::opcode<isa::Opcode::sty_extended>(),
                      address(product),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(product + 2),
                      isa::opcode<isa::Opcode::ldd_extended>(),
                      address(product + 1));
}

consteval auto butterfly(const Address u, const Address v, const Address coefficient) {
  return isa::program(multiply(v, coefficient),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(real),
                      multiply(v + 2, coefficient + 2),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(term),
                      isa::opcode<isa::Opcode::ldd_extended>(),
                      address(real),
                      isa::opcode<isa::Opcode::subd_extended>(),
                      address(term),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(real),
                      multiply(v, coefficient + 2),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(imaginary),
                      multiply(v + 2, coefficient),
                      isa::opcode<isa::Opcode::addd_extended>(),
                      address(imaginary),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(imaginary),
                      isa::opcode<isa::Opcode::ldd_extended>(),
                      address(u),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(previous),
                      isa::opcode<isa::Opcode::addd_extended>(),
                      address(real),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(u),
                      isa::opcode<isa::Opcode::ldd_extended>(),
                      address(previous),
                      isa::opcode<isa::Opcode::subd_extended>(),
                      address(real),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(v),
                      isa::opcode<isa::Opcode::ldd_extended>(),
                      address(u + 2),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(previous),
                      isa::opcode<isa::Opcode::addd_extended>(),
                      address(imaginary),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(u + 2),
                      isa::opcode<isa::Opcode::ldd_extended>(),
                      address(previous),
                      isa::opcode<isa::Opcode::subd_extended>(),
                      address(imaginary),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(v + 2));
}

struct Butterfly {
  unsigned u, v, coefficient;
};

consteval auto schedule() {
  std::array<Butterfly, butterfly_count> result{};
  unsigned index = 0;
  for (unsigned width = 2; width <= points; width *= 2) {
    for (unsigned first = 0; first < points; first += width) {
      for (unsigned lane = 0; lane < width / 2; ++lane) {
        result[index++] = {.u = first + lane,
                           .v = first + lane + (width / 2),
                           .coefficient = lane * points / width};
      }
    }
  }
  return result;
}

inline constexpr auto operations = schedule();

consteval auto assemble() {
  constexpr auto size = butterfly(data, data + 4, twiddles).size();
  std::array<Byte, (butterfly_count * size) + isa::stop.size()> bytes{};
  std::size_t offset = 0;
  for (const auto operation : operations) {
    const auto body = butterfly(static_cast<Address>(data + (operation.u * 4)),
                                static_cast<Address>(data + (operation.v * 4)),
                                static_cast<Address>(twiddles + (operation.coefficient * 4)));
    for (const auto byte : body) { bytes[offset++] = byte; }
  }
  for (const auto byte : isa::stop) { bytes[offset++] = byte; }
  return bytes;
}

inline constexpr auto program = assemble();
inline constexpr Address stop_pc = entry + program.size() - isa::stop.size();
inline constexpr Address return_pc = entry + program.size();

inline void word(Image &ram, const Address at, const Address value) {
  ram[at] = static_cast<Byte>(value >> byte_bits);
  ram[at + 1U] = static_cast<Byte>(value & byte_mask);
}

inline Address word(const Image &ram, const Address at) {
  return static_cast<Address>((unsigned{ram[at]} << byte_bits) | ram[at + 1U]);
}

inline Registers initial(const Byte ccr) {
  constexpr Address initial_d = 0x1234;
  constexpr Address initial_x = 0x1357;
  constexpr Address initial_y = 0x2468;
  return {.pc = entry, .d = initial_d, .x = initial_x, .y = initial_y, .sp = stack, .ccr = ccr};
}

inline void reset(Image &ram, const Samples &input) {
  for (unsigned index = 0; index < points; ++index) {
    unsigned reversed = 0;
    for (unsigned bit = 0; bit < stages; ++bit) {
      reversed = (reversed << 1U) | ((index >> bit) & 1U);
    }
    word(ram, static_cast<Address>(data + (reversed * 4)), input[std::size_t{index} * 2]);
    word(ram, static_cast<Address>(data + (reversed * 4) + 2), input[(std::size_t{index} * 2) + 1]);
  }
  for (unsigned offset = 0; offset < scratch_bytes; ++offset) { ram[product + offset] = 0; }
}

inline void frame(Image &ram, const Registers state) {
  const std::array<Byte, frame_bytes> bytes{state.ccr,
                                            static_cast<Byte>(state.d & byte_mask),
                                            static_cast<Byte>(state.d >> byte_bits),
                                            static_cast<Byte>(state.x >> byte_bits),
                                            static_cast<Byte>(state.x & byte_mask),
                                            static_cast<Byte>(state.y >> byte_bits),
                                            static_cast<Byte>(state.y & byte_mask),
                                            static_cast<Byte>(state.pc >> byte_bits),
                                            static_cast<Byte>(state.pc & byte_mask)};
  for (unsigned index = 0; index < bytes.size(); ++index) {
    ram[stack - frame_bytes + index] = bytes[index];
  }
}
}
