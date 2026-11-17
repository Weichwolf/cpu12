// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "program.hpp"

#include <array>
#include <string>

namespace cpu12::bench {
inline constexpr Address control_state = 0x60, checksum = 0x62, passes = 0x64;
inline constexpr Address handler_table = 0x100;
inline constexpr Address copy_source = 0x1000, copy_destination = 0x3000;
inline constexpr unsigned copy_words = 4096, copy_passes = 512, control_passes = 16;
inline constexpr unsigned y_nonzero = 0x26, x_post_increment_two = 0x31,
                          y_post_increment_two = 0x71;
inline constexpr unsigned indirect_d_x = 0xe7;

inline Program control_program() {
  Program p;
  p.label("entry");
  p.word(Op::ldx_immediate, handler_table);
  p.label("pass");
  p.word(Op::ldy_immediate, 0); // DBNE wraps once: 65536 transitions.
  p.label("transition");
  p.word(Op::ldd_extended, control_state);
  p.op(Op::asld);
  p.byte(Op::jsr_indexed, indirect_d_x);
  p.word(Op::ldd_extended, control_state);
  p.word(Op::addd_immediate, 3);
  p.byte(Op::andb_immediate, 7);
  p.word(Op::std_extended, control_state);
  p.dbne(y_nonzero, "transition");
  p.word(Op::ldd_extended, passes);
  p.word(Op::subd_immediate, 1);
  p.word(Op::std_extended, passes);
  p.branch(Op::bne_relative, "pass");
  p.op(Op::wai);
  p.label("end");
  p.branch(Op::bra_relative, "end");
  for (unsigned state = 0; state < 8; ++state) {
    p.label("handler" + std::to_string(state));
    p.root("handler" + std::to_string(state));
    p.word(Op::ldd_extended, checksum);
    p.word(Op::addd_immediate, state);
    p.byte(Op::eorb_immediate, state + 1);
    p.call("common");
    p.word(Op::std_extended, checksum);
    p.op(Op::rts);
  }
  p.label("common");
  p.word(Op::addd_immediate, 3);
  p.op(Op::rts);
  p.resolve();
  return p;
}

inline Program copy_program() {
  Program p;
  p.label("entry");
  p.word(Op::ldx_immediate, copy_source);
  p.word(Op::ldy_immediate, copy_destination);
  p.label("word");
  p.byte(Op::ldd_indexed, x_post_increment_two);
  p.byte(Op::std_indexed, y_post_increment_two);
  p.word(Op::addd_extended, checksum);
  p.word(Op::std_extended, checksum);
  p.word(Op::cpx_immediate, copy_source + (copy_words * 2));
  p.branch(Op::bne_relative, "word");
  p.word(Op::ldd_extended, passes);
  p.word(Op::subd_immediate, 1);
  p.word(Op::std_extended, passes);
  p.branch(Op::bne_relative, "entry");
  p.op(Op::wai);
  p.label("end");
  p.branch(Op::bra_relative, "end");
  p.resolve();
  return p;
}
}
