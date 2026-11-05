// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"
#include "memory_unary_model.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

namespace {
using namespace cpu12;
using namespace cpu12::test::memory_unary;
namespace indexed = cpu12::test::indexed;
constexpr std::array<unsigned, 8> operands{0, 1, 0x7f, 0x80, 0xfe, 0xff, 0x10, 0x55};

struct Point {
  Operation operation;
  Registers before, updated;
  std::uint16_t address;
  unsigned data;
};

void one(Core &core,
         const std::span<Byte> ram,
         const std::span<const Byte> code,
         const Point point) {
  const auto result = prediction(point.operation, point.before, point.data);
  ram[point.address] = static_cast<Byte>(point.data);
  const auto left = static_cast<std::uint16_t>(point.address + 0xffffU);
  const auto right = static_cast<std::uint16_t>(point.address + 1U);
  constexpr Byte left_guard = 0xa5;
  constexpr Byte right_guard = 0x5a;
  ram[left] = left_guard;
  ram[right] = right_guard;
  retired(core, code, point.before, expected(point.before, point.updated, code, result));
  require(ram[point.address] == (point.operation.write ? result.data : point.data) &&
              ram[left] == left_guard && ram[right] == right_guard,
          "Unary memory operation accesses only its actual byte and preserves adjacent data");
}

void complete_matrix(Core &core, const std::span<Byte> ram, const std::uint16_t address) {
  for (const auto operation : operations) {
    const std::array code{
        operation.extended, static_cast<Byte>(address >> byte_bits), static_cast<Byte>(address)};
    for (unsigned data = 0; data <= byte_mask; ++data) {
      for (unsigned flags = 0; flags <= byte_mask; ++flags) {
        auto before = seed;
        before.ccr = static_cast<Byte>(flags);
        one(core,
            ram,
            code,
            {.operation = operation,
             .before = before,
             .updated = before,
             .address = address,
             .data = data});
      }
    }
  }
}

void indexed_cases(Core &core, const std::span<Byte> ram) {
  constexpr std::array<Registers, 3> states{
      {{.pc = 0xfffe, .d = 0x80ff, .x = 0, .y = 0x7fff, .sp = 0xffff, .ccr = 0xff},
       {.pc = 0, .d = 0x7f80, .x = 0xffff, .y = 0, .sp = 0x8000, .ccr = 0x20},
       {.pc = 0x7fff, .d = 0x0010, .x = 0x8000, .y = 0xffff, .sp = 1, .ccr = 0xd1}}};
  constexpr std::uint16_t first_target = 0x2000;
  constexpr std::uint16_t second_target = 0x3000;
  for (const auto form : indexed::forms()) {
    for (const auto before : states) {
      const auto pointer = indexed::pointer(form, before);
      const bool overlap = pointer >= first_target - 2U && pointer <= first_target + 1U;
      const auto indirect_target = overlap ? second_target : first_target;
      const auto target = form.indirect ? indirect_target : pointer;
      for (const auto operation : operations) {
        const auto code = indexed::encode(form, operation.indexed);
        for (const auto data : operands) {
          if (form.indirect) {
            ram[pointer] = static_cast<Byte>(target >> byte_bits);
            ram[static_cast<std::uint16_t>(pointer + 1U)] = static_cast<Byte>(target);
          }
          one(core,
              ram,
              code.view(),
              {.operation = operation,
               .before = before,
               .updated = before,
               .address = target,
               .data = data});
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
  constexpr unsigned auto_mode = 0x20;
  constexpr unsigned post = 0x10;
  const auto raw = point.delta > 0 ? static_cast<unsigned>(point.delta - 1)
                                   : 16U - static_cast<unsigned>(-point.delta);
  const auto postbyte =
      static_cast<Byte>((point.base << 6U) | auto_mode | (point.pre ? 0U : post) | raw);
  for (const auto operation : operations) {
    for (const unsigned flags : {0U, 1U, 0xfeU, 0xffU}) {
      auto before = seed;
      before.*fields[point.base] = (flags & 1U) == 0 ? 0 : static_cast<std::uint16_t>(byte_mask);
      before.ccr = static_cast<Byte>(flags);
      auto updated = before;
      updated.*fields[point.base] =
          static_cast<std::uint16_t>(static_cast<int>(before.*fields[point.base]) + point.delta);
      const auto address = point.pre ? updated.*fields[point.base] : before.*fields[point.base];
      for (const auto data : operands) {
        one(core,
            ram,
            std::array{operation.indexed, postbyte},
            {.operation = operation,
             .before = before,
             .updated = updated,
             .address = address,
             .data = data});
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

void native_matrix() {
  std::array<Byte, memory_bytes> ram{};
  Core core(Memory::unified(ram));
  constexpr std::uint16_t address = 0xffff;
  const auto start = checked;
  complete_matrix(core, ram, address);
  indexed_cases(core, ram);
  auto_cases(core, ram);
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
              core.statistics().native_instructions == checked - start,
          "Full RAM unary operations execute as direct native accesses without ISA or bus helpers");
}

void partial_native_matrix() {
  std::array<Byte, memory_bytes> ram{};
  constexpr std::uint16_t first = 0x2000;
  constexpr std::uint16_t size = 0x100;
  constexpr std::uint16_t last = first + size - 1U;
  Core core(Memory::window({.first = first, .bytes = std::span(ram).subspan(first, size)}));
  complete_matrix(core, ram, last);
  for (const auto operation : operations) {
    for (const auto address : {first, last}) {
      auto before = seed;
      before.x = address;
      const std::array code{operation.indexed, Byte{0}};
      for (const auto data : operands) {
        one(core,
            ram,
            code,
            {.operation = operation,
             .before = before,
             .updated = before,
             .address = address,
             .data = data});
      }
    }
  }
  require(
      core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
      "Partial RAM admission occurs before the native read/modify/write and retains result flags");
}

void refusals() {
  Core core;
  core.set_registers(seed);
  constexpr std::array<std::array<Byte, 4>, 3> invalid{
      {{0x60, 0xe3, 0, 0}, {0x70, 0x20, 0, 0}, {0xf7, 0x20, 0, 0}}};
  for (const auto code : invalid) {
    const auto result = core.run_instruction(std::span(code).first(1));
    require(result.fault == Fault::unsupported_instruction && result.retired == 0 &&
                core.registers() == seed,
            "Truncated unary memory captures refuse before any real bus effect");
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Malformed memory captures make no external request");
}
}

int main() {
  try {
    native_matrix();
    partial_native_matrix();
    external_cases();
    refusals();
    std::cout
        << "Verified " << checked
        << " native memory-unary cases, CLR write-only/TST read-only, staged flags/index updates, "
           "partial RAM and explicit external phase faults.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
