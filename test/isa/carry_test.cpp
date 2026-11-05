// SPDX-License-Identifier: GPL-3.0-only
#include "alu_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace cpu12::test::alu {
namespace {
constexpr std::array<Byte, 4> opcodes{0x89, 0xc9, 0x82, 0xc2};
constexpr std::array<unsigned, 3> upper_flags{0, 0xd0, 0xfe};
constexpr unsigned add_opcode = 0x89;
constexpr unsigned b_bit = 0x40;
constexpr unsigned sign = 0x80;
constexpr unsigned range = 256;
constexpr std::array<unsigned, 8> operands{0, 1, 15, 16, 0x7f, 0x80, 0xfe, 0xff};

int signed_byte(const unsigned value) {
  return value < sign ? static_cast<int>(value) : static_cast<int>(value) - static_cast<int>(range);
}

struct Input {
  unsigned opcode, left, right, ccr;
};

Registers expected(const Input input, Registers before) {
  const bool add = (input.opcode & (byte_mask ^ b_bit)) == add_opcode;
  const auto cin = input.ccr & carry;
  const auto unsigned_value =
      static_cast<int>(input.left) +
      (add ? static_cast<int>(input.right + cin) : -static_cast<int>(input.right + cin));
  const auto signed_value =
      signed_byte(input.left) + (add ? signed_byte(input.right) + static_cast<int>(cin)
                                     : -signed_byte(input.right) - static_cast<int>(cin));
  const auto value = static_cast<unsigned>(unsigned_value + static_cast<int>(range)) % range;
  const bool carry_out = add ? unsigned_value >= static_cast<int>(range) : unsigned_value < 0;
  const unsigned c = carry_out ? carry : 0U;
  const unsigned v =
      signed_value < -static_cast<int>(sign) || signed_value >= static_cast<int>(sign) ? overflow
                                                                                       : 0U;
  const auto changed = negative | zero | overflow | carry | (add ? half : 0U);
  const unsigned h = add && (input.left % 16U) + (input.right % 16U) + cin >= 16U ? half : 0U;
  before = accumulator(before, (input.opcode & b_bit) != 0, value);
  before.ccr = static_cast<Byte>((input.ccr & (byte_mask ^ changed)) | c | v | h |
                                 (value >= sign ? negative : 0U) | (value == 0 ? zero : 0U));
  return before;
}

void one_case(Core &core,
              const std::span<const Byte> code,
              const Registers before,
              Registers updated,
              const unsigned operand) {
  const bool b = (code[0] & b_bit) != 0;
  const auto left = b ? before.d & byte_mask : before.d >> byte_bits;
  constexpr unsigned addressing_mask = 0xcf;
  auto wanted = expected({.opcode = static_cast<unsigned>(code[0]) & addressing_mask,
                          .left = left,
                          .right = operand,
                          .ccr = before.ccr},
                         updated);
  wanted.pc = static_cast<std::uint16_t>(before.pc + code.size());
  retired(core, code, before, wanted);
}

void immediate(Core &core) {
  for (const auto opcode : opcodes) {
    const bool b = (opcode & b_bit) != 0;
    for (unsigned right = 0; right < range; ++right) {
      const std::array code{opcode, static_cast<Byte>(right)};
      for (unsigned left = 0; left < range; ++left) {
        for (const auto flags : upper_flags) {
          for (const unsigned cin : {0U, 1U}) {
            auto before = accumulator(seed, b, left);
            before.ccr = static_cast<Byte>((flags & (byte_mask ^ carry)) | cin);
            one_case(core, code, before, before, right);
          }
        }
      }
    }
  }
}

void constant_memory(Core &core, const std::span<Byte> ram) {
  constexpr std::uint16_t extended_address = 0xffff;
  constexpr Byte direct_address = 0xff;
  for (const auto opcode : opcodes) {
    for (const auto value : operands) {
      for (const auto flags : upper_flags) {
        auto before = seed;
        before.ccr = static_cast<Byte>(flags);
        for (const unsigned cin : {0U, 1U}) {
          before.ccr = static_cast<Byte>((flags & (byte_mask ^ carry)) | cin);
          ram[extended_address] = static_cast<Byte>(value);
          ram[direct_address] = static_cast<Byte>(value);
          constexpr unsigned direct_delta = 0x10;
          constexpr unsigned extended_delta = 0x30;
          const std::array direct{static_cast<Byte>(opcode + direct_delta), direct_address};
          const std::array extended{
              static_cast<Byte>(opcode + extended_delta), Byte{0xff}, Byte{0xff}};
          one_case(core, direct, before, before, value);
          one_case(core, extended, before, before, value);
        }
      }
    }
  }
}

void indexed_cases(Core &core, const std::span<Byte> ram) {
  constexpr std::array<Registers, 3> states{
      {{.pc = 0xfffe, .d = 0x80ff, .x = 0xffff, .y = 0, .sp = 1, .ccr = 0xff},
       {.pc = 0, .d = 0x7f80, .x = 0, .y = 0xffff, .sp = 0x8000, .ccr = 0x20},
       {.pc = 0x7fff, .d = 0x0010, .x = 0x8000, .y = 0x7fff, .sp = 0xffff, .ccr = 0xd1}}};
  constexpr unsigned delta = 0x20;
  for (const auto form : indexed::forms()) {
    for (const auto before : states) {
      const auto pointer = indexed::pointer(form, before);
      constexpr std::uint16_t first_target = 0x2000;
      constexpr std::uint16_t second_target = 0x3000;
      const auto indirect_target =
          pointer == first_target || indexed::pointer(form, before) == first_target - 1U
              ? second_target
              : first_target;
      const auto target = form.indirect ? indirect_target : pointer;
      for (const auto opcode : opcodes) {
        for (const auto value : operands) {
          if (form.indirect) {
            ram[pointer] = static_cast<Byte>(target >> byte_bits);
            ram[static_cast<std::uint16_t>(pointer + 1U)] = static_cast<Byte>(target);
          }
          ram[target] = static_cast<Byte>(value);
          one_case(core,
                   indexed::encode(form, static_cast<Byte>(opcode + delta)).view(),
                   before,
                   before,
                   value);
        }
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
  constexpr unsigned post = 0x10;
  constexpr unsigned auto_mode = 0x20;
  constexpr unsigned indexed_delta = 0x20;
  const auto raw = point.delta > 0 ? static_cast<unsigned>(point.delta - 1)
                                   : 16U - static_cast<unsigned>(-point.delta);
  const auto postbyte =
      static_cast<Byte>((point.base << 6U) | auto_mode | (point.pre ? 0U : post) | raw);
  for (const auto opcode : opcodes) {
    for (const auto flags : upper_flags) {
      for (const unsigned cin : {0U, 1U}) {
        auto before = seed;
        before.*fields[point.base] = cin == 0 ? 0 : static_cast<std::uint16_t>(range - 1U);
        before.ccr = static_cast<Byte>((flags & (byte_mask ^ carry)) | cin);
        auto updated = before;
        updated.*fields[point.base] =
            static_cast<std::uint16_t>(static_cast<int>(before.*fields[point.base]) + point.delta);
        const auto address = point.pre ? updated.*fields[point.base] : before.*fields[point.base];
        for (const auto value : operands) {
          ram[address] = static_cast<Byte>(value);
          one_case(core,
                   std::array{static_cast<Byte>(opcode + indexed_delta), postbyte},
                   before,
                   updated,
                   value);
        }
      }
    }
  }
}

void auto_cases(Core &core, const std::span<Byte> ram) {
  for (unsigned base = 0; base < 3; ++base) {
    for (int delta = -8; delta <= 8; ++delta) {
      if (delta == 0) { continue; }
      for (const bool pre : {false, true}) {
        auto_case(core, ram, {.base = base, .delta = delta, .pre = pre});
      }
    }
  }
}

struct Port {
  Core *core = nullptr;
  Registers before{};
  Byte data = 0;
  unsigned calls = 0;
  bool accept = false, observed = true;

  static bool read(void *owner, const std::uint16_t address, Byte &data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    constexpr std::uint16_t selected = 0x2000;
    if (address != selected) { return false; }
    ++port.calls;
    port.observed = port.observed && port.core->registers() == port.before;
    data = port.data;
#if defined(__x86_64__)
    asm volatile("stc" : : : "cc");
#endif
    return port.accept;
  }
};

void external() {
  constexpr unsigned extended_delta = 0x30;
  constexpr std::uint16_t address = 0x2000;
  constexpr Byte operand = 0xff;
  for (const auto opcode : opcodes) {
    for (const unsigned cin : {0U, 1U}) {
      Port port{.before = seed, .data = operand};
      port.before.ccr = static_cast<Byte>((seed.ccr & (byte_mask ^ carry)) | cin);
      Io io{.owner = &port, .read = Port::read};
      Core core(Memory::external(), &io);
      port.core = &core;
      core.set_registers(port.before);
      const std::array code{static_cast<Byte>(opcode + extended_delta), Byte{0x20}, Byte{0}};
      const auto failure = core.run_instruction(code);
      require(failure.fault == Fault::memory_read && failure.retired == 0 &&
                  failure.address == address && failure.value == operand &&
                  failure.access_size == 1 && core.registers() == port.before && port.calls == 1,
              "Rejected carry-ALU read retains actual context and original CPU");
      require(core.run_instruction(code).retired == 0 && port.calls == 1,
              "Rejected carry-ALU reads do not replay after cache invalidation");
      port.accept = true;
      one_case(core, code, port.before, port.before, operand);
      require(port.calls == 2 && port.observed && core.statistics().write_callbacks == 0,
              "Accepted carry-ALU uses the original guest carry after a real external callback");
    }
  }
}
}

void carry_operations() {
  std::array<Byte, address_bytes> ram{};
  Core core(Memory::unified(ram));
  const auto start = checked;
  immediate(core);
  constant_memory(core, ram);
  indexed_cases(core, ram);
  auto_cases(core, ram);
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
              core.statistics().native_instructions == checked - start,
          "Known-RAM ADC/SBC and all indexed forms execute without ISA or bus helpers");
  external();
}
}
