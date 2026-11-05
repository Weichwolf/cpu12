// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <stdexcept>

namespace cpu12::test::mandelbrot {
inline constexpr Address entry = 0x0200, data = 0x4000, stack = 0x9000;
inline constexpr Address cx = data, cy = data + 2, x = data + 4, y = data + 6;
inline constexpr Address xx = data + 8, yy = data + 10, product = data + 12;
inline constexpr unsigned byte_bits = 8, byte_mask = 255, word_mask = 65535, sign = 32768;
inline constexpr unsigned preserved_flags = 0x70;
inline constexpr unsigned radius_limit = 1024, instructions_per_iteration = 32;
using Image = std::array<Byte, address_bytes>;

consteval auto address(const Address value) {
  return std::array<Byte, 2>{static_cast<Byte>(value >> byte_bits),
                             static_cast<Byte>(value & byte_mask)};
}

consteval auto square(const Address source, const Address target) {
  return isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                      address(source),
                      isa::opcode<isa::Opcode::ldy_extended>(),
                      address(source),
                      isa::emuls,
                      isa::opcode<isa::Opcode::sty_extended>(),
                      address(product),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(product + 2),
                      isa::opcode<isa::Opcode::ldd_extended>(),
                      address(product + 1),
                      isa::opcode<isa::Opcode::std_extended>(),
                      address(target));
}

inline constexpr auto prefix = isa::program(square(x, xx),
                                            square(y, yy),
                                            isa::opcode<isa::Opcode::addd_extended>(),
                                            address(xx),
                                            isa::opcode<isa::Opcode::cpd_immediate>(),
                                            address(radius_limit),
                                            isa::opcode<isa::Opcode::bhs_relative>());
inline constexpr auto tail = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                                          address(x),
                                          isa::opcode<isa::Opcode::ldy_extended>(),
                                          address(y),
                                          isa::emuls,
                                          isa::opcode<isa::Opcode::sty_extended>(),
                                          address(product),
                                          isa::opcode<isa::Opcode::std_extended>(),
                                          address(product + 2),
                                          isa::opcode<isa::Opcode::ldd_extended>(),
                                          address(product + 1),
                                          isa::asld,
                                          isa::opcode<isa::Opcode::addd_extended>(),
                                          address(cy),
                                          isa::opcode<isa::Opcode::std_extended>(),
                                          address(y),
                                          isa::opcode<isa::Opcode::ldd_extended>(),
                                          address(xx),
                                          isa::opcode<isa::Opcode::subd_extended>(),
                                          address(yy),
                                          isa::opcode<isa::Opcode::addd_extended>(),
                                          address(cx),
                                          isa::opcode<isa::Opcode::std_extended>(),
                                          address(x),
                                          isa::dex,
                                          isa::opcode<isa::Opcode::bne_relative>());
inline constexpr std::array<Byte, 1> exit_offset{static_cast<Byte>(tail.size() + 1)};
inline constexpr std::array<Byte, 1> back_offset{
    static_cast<Byte>(256 - (prefix.size() + tail.size() + 2))};
inline constexpr auto program = isa::program(prefix, exit_offset, tail, back_offset, isa::stop);
inline constexpr Address tail_entry = entry + prefix.size() + 1;
inline constexpr Address stop_entry = entry + program.size() - isa::stop.size();
inline constexpr Address return_pc = entry + program.size();

struct Point {
  Address real, imaginary, limit = 32;
  Byte ccr = 0;
};

struct Result {
  Registers registers;
  unsigned retired = 0;
};

inline void word(Image &ram, const Address at, const Address value) {
  ram[at] = static_cast<Byte>(value >> byte_bits);
  ram[at + 1U] = static_cast<Byte>(value & byte_mask);
}

inline Address word(const Image &ram, const Address at) {
  return static_cast<Address>((unsigned{ram[at]} << byte_bits) | ram[at + 1U]);
}

inline Registers initial(const Point point) {
  return {.pc = entry, .x = point.limit, .sp = stack, .ccr = point.ccr};
}

inline void reset(Image &ram, const Point point) {
  constexpr unsigned disable_stop = 0x80;
  if (point.limit == 0 || (point.ccr & disable_stop) != 0) {
    throw std::invalid_argument("Mandelbrot requires a positive limit and enabled STOP");
  }
  for (unsigned offset = 0; offset < 16; ++offset) { ram[data + offset] = 0; }
  word(ram, cx, point.real);
  word(ram, cy, point.imaginary);
}

inline void frame(Image &ram, const Registers state) {
  const std::array<Byte, 9> bytes{state.ccr,
                                  static_cast<Byte>(state.d & byte_mask),
                                  static_cast<Byte>(state.d >> byte_bits),
                                  static_cast<Byte>(state.x >> byte_bits),
                                  static_cast<Byte>(state.x & byte_mask),
                                  static_cast<Byte>(state.y >> byte_bits),
                                  static_cast<Byte>(state.y & byte_mask),
                                  static_cast<Byte>(state.pc >> byte_bits),
                                  static_cast<Byte>(state.pc & byte_mask)};
  for (unsigned index = 0; index < bytes.size(); ++index) {
    ram[stack - bytes.size() + index] = bytes[index];
  }
}
}
