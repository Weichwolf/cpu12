// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"
#include "stack_model.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace cpu12::test::stack {
namespace {
constexpr std::array<std::uint16_t, 6> positions{0, 1, 2, 0x7fff, 0xfffe, 0xffff};
constexpr std::array<std::uint16_t, 8> targets{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xfffe, 0xffff};
constexpr unsigned flags_count = 256;
constexpr Byte indexed_jsr = 0x15;
constexpr Byte extended_jsr = 0x16;
constexpr Byte direct_jsr = 0x17;

void relative_calls(Core &core, const std::span<Byte> ram) {
  constexpr Byte bsr = 7;
  for (int delta = -128; delta < 128; ++delta) {
    const std::array<Byte, 2> code{bsr, static_cast<Byte>(delta)};
    for (const auto seed : seeds) {
      for (const auto position : positions) {
        auto before = seed;
        before.sp = position;
        auto wanted = before;
        wanted.sp = static_cast<std::uint16_t>(static_cast<int>(position) - 2);
        wanted.pc = static_cast<std::uint16_t>(static_cast<int>(before.pc) + 2 + delta);
        call(core, ram, code, before, wanted);
      }
    }
  }
}

void fixed_case(Core &core,
                const std::span<Byte> ram,
                const std::span<const Byte> code,
                const std::uint16_t target) {
  for (const auto seed : seeds) {
    for (const auto position : positions) {
      for (unsigned flags = 0; flags < flags_count; ++flags) {
        auto before = seed;
        before.sp = position;
        before.ccr = static_cast<Byte>(flags);
        auto wanted = before;
        wanted.sp = static_cast<std::uint16_t>(static_cast<int>(position) - 2);
        wanted.pc = target;
        call(core, ram, code, before, wanted);
      }
    }
  }
}

void fixed_calls(Core &core, const std::span<Byte> ram) {
  for (unsigned target = 0; target <= byte_mask; ++target) {
    const std::array<Byte, 2> code{direct_jsr, static_cast<Byte>(target)};
    fixed_case(core, ram, code, static_cast<std::uint16_t>(target));
  }
  for (const auto target : targets) {
    const std::array<Byte, 3> code{
        extended_jsr, static_cast<Byte>(target >> byte_bits), static_cast<Byte>(target)};
    fixed_case(core, ram, code, target);
  }
}

void indexed_calls(Core &core, const std::span<Byte> ram) {
  constexpr std::uint16_t indirect_target = 0x4321;
  for (const auto form : indexed::forms()) {
    const auto code = indexed::encode(form, indexed_jsr);
    for (const auto seed : seeds) {
      const auto pointer = indexed::pointer(form, seed);
      if (form.indirect) { word(ram, pointer, indirect_target); }
      for (unsigned flags = 0; flags < flags_count; ++flags) {
        auto before = seed;
        before.ccr = static_cast<Byte>(flags);
        auto wanted = before;
        wanted.pc = form.indirect ? indirect_target : pointer;
        wanted.sp = static_cast<std::uint16_t>(static_cast<int>(before.sp) - 2);
        // Each stack write can alias this pointer; restore the authored input.
        if (form.indirect) { word(ram, pointer, indirect_target); }
        call(core, ram, code.view(), before, wanted);
      }
    }
  }
}

struct Auto {
  unsigned base;
  int delta;
  bool pre;
};

void auto_case(Core &core, const std::span<Byte> ram, const Auto point) {
  constexpr std::array<indexed::Field, 3> fields{&Registers::x, &Registers::y, &Registers::sp};
  constexpr unsigned auto_mode = 0x20;
  constexpr unsigned post_mode = 0x10;
  const auto encoded = point.delta > 0 ? static_cast<unsigned>(point.delta - 1)
                                       : 16U - static_cast<unsigned>(-point.delta);
  const std::array<Byte, 2> code{
      indexed_jsr,
      static_cast<Byte>((point.base << 6U) | auto_mode | (point.pre ? 0U : post_mode) | encoded)};
  for (const auto seed : seeds) {
    for (unsigned flags = 0; flags < flags_count; ++flags) {
      auto before = seed;
      before.ccr = static_cast<Byte>(flags);
      auto wanted = before;
      wanted.*fields[point.base] =
          static_cast<std::uint16_t>(static_cast<int>(before.*fields[point.base]) + point.delta);
      wanted.pc = point.pre ? wanted.*fields[point.base] : before.*fields[point.base];
      wanted.sp = static_cast<std::uint16_t>(static_cast<int>(wanted.sp) - 2);
      call(core, ram, code, before, wanted);
    }
  }
}

void auto_calls(Core &core, const std::span<Byte> ram) {
  for (unsigned base = 0; base < 3; ++base) {
    for (int delta = -8; delta <= 8; ++delta) {
      if (delta == 0) { continue; }
      for (const bool pre : {false, true}) {
        auto_case(core, ram, {.base = base, .delta = delta, .pre = pre});
      }
    }
  }
}

void overlapping_pointers(Core &core, const std::span<Byte> ram) {
  constexpr std::uint16_t target = 0x1234;
  constexpr std::array<std::array<Byte, 4>, 2> codes{
      {{indexed_jsr, 0xf3, 0xff, 0xfe}, {indexed_jsr, 0xe3, 0, 0}}};
  for (const auto seed : seeds) {
    auto before = seed;
    before.x = static_cast<std::uint16_t>(static_cast<int>(before.sp) - 2);
    auto wanted = before;
    wanted.pc = target;
    wanted.sp = before.x;
    for (const auto code : codes) {
      word(ram, before.x, target);
      call(core, ram, code, before, wanted);
    }
  }
}
}

void calls() {
  std::array<Byte, memory_bytes> ram{};
  Core core(Memory::unified(ram));
  const auto start = checked;
  relative_calls(core, ram);
  fixed_calls(core, ram);
  indexed_calls(core, ram);
  auto_calls(core, ram);
  overlapping_pointers(core, ram);
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
              core.statistics().native_instructions == checked - start,
          "Known-RAM calls use generated arithmetic/access instructions without ISA helpers");
}
}
