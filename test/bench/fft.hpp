// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "filters.hpp"
#include "program.hpp"

namespace cpu12::bench {
inline constexpr unsigned fft_points = 256, fft_repetitions = 512;
inline constexpr Address fft_data = 0x2000, fft_twiddles = 0x3000;
inline constexpr Address fft_half = 0x80, fft_step = 0x82, fft_group = 0x84;
inline constexpr Address fft_upper = 0x86, fft_lower = 0x88, fft_coefficient = 0x8a;
inline constexpr Address fft_limit = 0x8c, fft_real = 0x8e, fft_imaginary = 0x90, fft_old = 0x92;
inline constexpr unsigned twiddle_bytes = 6, complex_bytes = 4, y_minus_four = 0x5c;

inline Program fft_program() {
  Program p;
  p.label("entry");
  p.word(Op::ldd_immediate, complex_bytes);
  p.word(Op::std_extended, fft_half);
  p.word(Op::ldd_immediate, (fft_points / 2) * twiddle_bytes);
  p.word(Op::std_extended, fft_step);
  p.label("stage");
  p.word(Op::ldd_immediate, fft_data);
  p.word(Op::std_extended, fft_group);
  p.label("group");
  p.word(Op::ldd_extended, fft_group);
  p.word(Op::std_extended, fft_upper);
  p.word(Op::addd_extended, fft_half);
  p.word(Op::std_extended, fft_lower);
  p.word(Op::std_extended, fft_limit);
  p.word(Op::ldd_immediate, fft_twiddles);
  p.word(Op::std_extended, fft_coefficient);
  p.label("butterfly");
  p.word(Op::ldd_immediate, 0);
  p.word(Op::std_extended, accumulator);
  p.word(Op::std_extended, accumulator + 2);
  p.word(Op::ldx_extended, fft_lower);
  p.word(Op::ldy_extended, fft_coefficient);
  p.word(emacs, accumulator);
  p.byte(Op::leax_indexed, x_plus_two);
  p.byte(Op::leay_indexed, y_plus_four);
  p.word(emacs, accumulator);
  p.call("quantize");
  p.word(Op::std_extended, fft_real);
  p.word(Op::ldd_immediate, 0);
  p.word(Op::std_extended, accumulator);
  p.word(Op::std_extended, accumulator + 2);
  p.byte(Op::leay_indexed, y_minus_four);
  p.word(emacs, accumulator);
  p.byte(Op::leax_indexed, x_minus_two);
  p.byte(Op::leay_indexed, y_plus_two);
  p.word(emacs, accumulator);
  p.call("quantize");
  p.word(Op::std_extended, fft_imaginary);
  p.word(Op::ldx_extended, fft_upper);
  p.word(Op::ldy_extended, fft_lower);
  for (unsigned component = 0; component < 2; ++component) {
    p.byte(Op::ldd_indexed, component * 2);
    p.word(Op::std_extended, fft_old);
    p.word(Op::addd_extended, fft_real + (component * 2));
    p.op(Op::asra);
    p.op(Op::rorb);
    p.byte(Op::std_indexed, component * 2);
    p.word(Op::ldd_extended, fft_old);
    p.word(Op::subd_extended, fft_real + (component * 2));
    p.op(Op::asra);
    p.op(Op::rorb);
    p.byte(Op::std_indexed, (component * 2) + 0x40); // offset,Y
  }
  p.byte(Op::leax_indexed, complex_bytes);
  p.word(Op::stx_extended, fft_upper);
  p.byte(Op::leay_indexed, 0x40 + complex_bytes);
  p.word(Op::sty_extended, fft_lower);
  p.word(Op::ldd_extended, fft_coefficient);
  p.word(Op::addd_extended, fft_step);
  p.word(Op::std_extended, fft_coefficient);
  p.word(Op::cpx_extended, fft_limit);
  p.long_branch(Op::bne_relative, "butterfly");
  p.word(Op::sty_extended, fft_group);
  p.word(Op::cpy_immediate, fft_data + (fft_points * complex_bytes));
  p.long_branch(Op::bne_relative, "group");
  p.word(Op::ldd_extended, fft_step);
  p.op(Op::lsrd);
  p.word(Op::std_extended, fft_step);
  p.word(Op::ldd_extended, fft_half);
  p.op(Op::asld);
  p.word(Op::std_extended, fft_half);
  p.word(Op::cpd_immediate, fft_points * complex_bytes);
  p.long_branch(Op::bne_relative, "stage");
  p.op(Op::wai);
  p.label("end");
  p.branch(Op::bra_relative, "end");
  quantizer(p);
  p.resolve();
  return p;
}
}
