// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "program.hpp"

namespace cpu12::bench {
inline constexpr Address accumulator = 0x40, cursor = 0x44, destination = 0x46;
inline constexpr Address coefficients = 0x100, input = 0x1000, output = 0x2000;
inline constexpr Address irq_counter = 0x48;
inline constexpr Address filter_input = 0x50, filtered = 0x52, state_cursor = 0x54;
inline constexpr Address filter_state = 0x180;
inline constexpr unsigned taps = 64, frames = 1024, blocks = 256;
inline constexpr unsigned d_nonzero = 0x24, x_nonzero = 0x25;
inline constexpr unsigned x_minus_two = 0x1e, x_plus_two = 2, y_plus_two = 0x42;
inline constexpr unsigned x_post_two = 0x31, y_plus_four = 0x44, y_minus_two = 0x5e;
inline constexpr Op emacs = static_cast<Op>(0x1812);

// Shared Q1.30 -> Q1.15 conversion. Bias is 0x3fff + quotient parity.
// Carry propagates through both words; signed clamps precede extraction.
inline void quantizer(Program &p) {
  constexpr unsigned low_bias = 0xff;
  constexpr unsigned high_bias = 0x3f;
  constexpr unsigned maximum_high = 0x3fff;
  constexpr unsigned minimum_high = 0xc000;
  constexpr unsigned positive_rail = 0x7fff;
  constexpr unsigned negative_rail = 0x8000;
  p.label("quantize");
  p.word(Op::ldd_extended, accumulator + 2);
  p.op(Op::asld);
  p.word(Op::ldd_extended, accumulator + 2);
  p.byte(Op::adcb_immediate, low_bias);
  p.byte(Op::adca_immediate, high_bias);
  p.word(Op::std_extended, accumulator + 2);
  p.word(Op::ldd_extended, accumulator);
  p.byte(Op::adcb_immediate, 0);
  p.byte(Op::adca_immediate, 0);
  p.word(Op::std_extended, accumulator);
  p.word(Op::cpd_immediate, maximum_high);
  p.branch(Op::bgt_relative, "positive");
  p.word(Op::cpd_immediate, minimum_high);
  p.branch(Op::blt_relative, "negative");
  p.word(Op::ldd_extended, accumulator + 2);
  p.op(Op::asld);
  p.word(Op::ldd_extended, accumulator);
  p.op(Op::rolb);
  p.op(Op::rola);
  p.op(Op::rts);
  p.label("positive");
  p.word(Op::ldd_immediate, positive_rail);
  p.op(Op::rts);
  p.label("negative");
  p.word(Op::ldd_immediate, negative_rail);
  p.op(Op::rts);
}

inline Program fir_program() {
  Program p;
  p.label("entry");
  p.word(Op::ldx_immediate, input + ((taps - 1) * 2));
  p.word(Op::stx_extended, cursor);
  p.word(Op::ldx_immediate, output);
  p.word(Op::stx_extended, destination);
  p.label("sample");
  p.word(Op::ldd_immediate, 0);
  p.word(Op::std_extended, accumulator);
  p.word(Op::std_extended, accumulator + 2);
  p.word(Op::ldx_extended, cursor);
  p.word(Op::ldy_immediate, coefficients);
  p.word(Op::ldd_immediate, taps);
  p.label("tap");
  p.word(emacs, accumulator);
  p.byte(Op::leax_indexed, x_minus_two);
  p.byte(Op::leay_indexed, y_plus_two);
  p.dbne(d_nonzero, "tap");
  p.call("quantize");
  p.word(Op::ldx_extended, destination);
  p.byte(Op::std_indexed, x_post_two); // 2,X+
  p.word(Op::stx_extended, destination);
  p.word(Op::ldx_extended, cursor);
  p.byte(Op::leax_indexed, x_plus_two);
  p.word(Op::stx_extended, cursor);
  p.word(Op::cpx_immediate, input + ((taps - 1 + frames) * 2));
  p.branch(Op::bne_relative, "sample");
  p.op(Op::wai);
  p.label("end");
  p.branch(Op::bra_relative, "end");
  quantizer(p);
  p.label("irq");
  p.word(Op::ldd_extended, irq_counter + 2);
  p.word(Op::addd_immediate, 1);
  p.word(Op::std_extended, irq_counter + 2);
  p.word(Op::ldd_extended, irq_counter);
  p.byte(Op::adcb_immediate, 0);
  p.byte(Op::adca_immediate, 0);
  p.word(Op::std_extended, irq_counter);
  p.op(Op::rti);
  p.resolve();
  return p;
}

inline Program biquad_program() {
  Program p;
  p.label("entry");
  p.word(Op::ldx_immediate, input);
  p.word(Op::stx_extended, cursor);
  p.word(Op::ldx_immediate, output);
  p.word(Op::stx_extended, destination);
  p.label("sample");
  p.word(Op::ldx_extended, cursor);
  p.byte(Op::ldd_indexed, x_post_two); // 2,X+
  p.word(Op::stx_extended, cursor);
  p.word(Op::std_extended, filter_input);
  p.word(Op::ldy_immediate, coefficients);
  p.word(Op::ldx_immediate, filter_state);
  p.word(Op::stx_extended, state_cursor);
  p.label("section");
  p.byte(Op::ldd_indexed, 0); // s1
  p.call("scale");
  p.word(Op::ldx_immediate, filter_input);
  p.word(emacs, accumulator); // b0*x + s1
  p.call("quantize");
  p.word(Op::std_extended, filtered);
  p.word(Op::ldx_extended, state_cursor);
  p.byte(Op::ldd_indexed, 2); // s2
  p.call("scale");
  p.word(Op::ldx_immediate, filter_input);
  p.byte(Op::leay_indexed, y_plus_two);
  p.word(emacs, accumulator); // b1*x + s2
  p.word(Op::ldx_immediate, filtered);
  p.byte(Op::leay_indexed, y_plus_four);
  p.word(emacs, accumulator); // -a1*y
  p.call("quantize");
  p.word(Op::ldx_extended, state_cursor);
  p.byte(Op::std_indexed, 0); // next s1
  p.word(Op::ldd_immediate, 0);
  p.word(Op::std_extended, accumulator);
  p.word(Op::std_extended, accumulator + 2);
  p.word(Op::ldx_immediate, filter_input);
  p.byte(Op::leay_indexed, y_minus_two); // -2,Y
  p.word(emacs, accumulator);            // b2*x
  p.word(Op::ldx_immediate, filtered);
  p.byte(Op::leay_indexed, y_plus_four);
  p.word(emacs, accumulator); // -a2*y
  p.call("quantize");
  p.word(Op::ldx_extended, state_cursor);
  p.byte(Op::std_indexed, 2); // next s2
  p.byte(Op::leax_indexed, 4);
  p.word(Op::stx_extended, state_cursor);
  p.byte(Op::leay_indexed, y_plus_two);
  p.word(Op::ldd_extended, filtered);
  p.word(Op::std_extended, filter_input);
  p.word(Op::cpy_immediate, coefficients + (4 * 5 * 2));
  p.branch(Op::bne_relative, "section");
  p.word(Op::ldx_extended, destination);
  p.byte(Op::std_indexed, x_post_two);
  p.word(Op::stx_extended, destination);
  p.word(Op::ldx_extended, cursor);
  p.word(Op::cpx_immediate, input + (frames * 2));
  // Long backward branch: the complete section body exceeds the short range.
  p.byte(Op::beq_relative, 3);
  p.word(Op::jmp_extended, p.at("sample"));
  p.op(Op::wai);
  p.label("end");
  p.branch(Op::bra_relative, "end");
  quantizer(p);
  p.label("scale");
  p.word(Op::std_extended, accumulator);
  p.word(Op::ldd_immediate, 0);
  p.word(Op::std_extended, accumulator + 2);
  p.word(Op::asr_extended, accumulator);
  p.word(Op::ror_extended, accumulator + 1);
  p.word(Op::ror_extended, accumulator + 2);
  p.op(Op::rts);
  p.resolve();
  return p;
}
}
