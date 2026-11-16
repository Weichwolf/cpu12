// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "interrupt_model.hpp"
#include "isa.hpp"

#include <array>

namespace cpu12::test::interrupts {
constexpr Address alternate = seed.pc + 32;

inline const NativeImage &return_image() {
  static const NativeImage image = [] {
    const std::array first{CodeSpan{.address = seed.pc, .words = isa::rti}};
    const std::array second{CodeSpan{.address = alternate, .words = isa::rti}};
    constexpr std::array first_entries{seed.pc};
    constexpr std::array second_entries{alternate};
    const std::array variants{CodeVariant{.id = 0, .code = first, .entries = first_entries},
                              CodeVariant{.id = 1, .code = second, .entries = second_entries}};
    return Core::compile({.variants = variants},
                         {.memory = {},
                          .interrupts = {},
                          .transitions = {},
                          .callbacks = {},
                          .bindings = {},
                          .topology = MemoryTopology::dynamic});
  }();
  return image;
}

class ReturnProgram {
public:
  [[nodiscard]] Registers bind(Core &core, Image &image, Registers before) {
    // Keep the real opcode outside the saved frame and both guard bytes for every SP.
    const auto distance = static_cast<Address>(seed.pc - before.sp + 1U);
    const auto variant = distance <= literal.size() + 1 ? 1U : 0U;
    before.pc = variant == 0 ? seed.pc : alternate;
    image[before.pc] = isa::rti[0];
    if (variant != active_) {
      core.install(return_image(), variant);
      active_ = variant;
    }
    core.set_registers(before);
    return before;
  }

private:
  unsigned active_ = 2;
};
}
