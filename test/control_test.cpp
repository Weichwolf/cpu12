// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace cpu12;
using namespace cpu12::test::indexed;
constexpr unsigned flags_count = 256;
constexpr unsigned byte_bits = 8;
constexpr std::array<Registers, 3> seeds{
    {{.pc = 0xfffe, .d = 0x80ff, .x = 0, .y = 0x7fff, .sp = 0xffff, .ccr = 0xff},
     {.pc = 0, .d = 0x7f80, .x = 0xffff, .y = 0, .sp = 0x8000, .ccr = 0x21},
     {.pc = 0x7fff, .d = 0, .x = 0x8000, .y = 0xffff, .sp = 1, .ccr = 0x12}}};
std::uint64_t checked = 0;

void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}

struct Branch {
  unsigned condition, ccr;
};

bool branch_taken(const Branch input) {
  const auto ccr = input.ccr;
  const bool carry = (ccr & 1U) != 0;
  const bool overflow = (ccr & 2U) != 0;
  const bool zero = (ccr & 4U) != 0;
  const bool negative = (ccr & 8U) != 0;
  // Independent manual truth table, not the emitter's transformed bit masks.
  const std::array truth{true,
                         false,
                         !carry && !zero,
                         carry || zero,
                         !carry,
                         carry,
                         !zero,
                         zero,
                         !overflow,
                         overflow,
                         !negative,
                         negative,
                         negative == overflow,
                         negative != overflow,
                         !zero && negative == overflow,
                         zero || negative != overflow};
  return truth[input.condition];
}

void retired(Core &core,
             const std::span<const Byte> code,
             const Registers before,
             const Registers wanted) {
  core.set_registers(before);
  const auto result = core.run_instruction(code);
  require(result.retired == 1 && result.fault == Fault::none && result.pc == before.pc &&
              result.opcode == code[0] && core.registers() == wanted,
          "Control transfer must match the independent full-register and original-PC expectation");
  ++checked;
}

void long_branches() {
  Core core;
  constexpr std::array<int, 9> displacements{-32768, -32767, -256, -1, 0, 1, 255, 32766, 32767};
  constexpr unsigned first_branch = 0x20;
  constexpr unsigned page = 0x18;
  for (unsigned condition = 0; condition < 16; ++condition) {
    for (const auto displacement : displacements) {
      const auto raw = static_cast<std::uint16_t>(displacement);
      const std::array<Byte, 4> code{page,
                                     static_cast<Byte>(first_branch + condition),
                                     static_cast<Byte>(raw >> byte_bits),
                                     static_cast<Byte>(raw)};
      for (const auto seed : seeds) {
        for (unsigned flags = 0; flags < flags_count; ++flags) {
          auto before = seed;
          before.ccr = static_cast<Byte>(flags);
          auto wanted = before;
          wanted.pc = static_cast<std::uint16_t>(
              static_cast<int>(before.pc) + 4 +
              (branch_taken({.condition = condition, .ccr = flags}) ? displacement : 0));
          retired(core, code, before, wanted);
        }
      }
    }
  }
  require(core.statistics().compiled_entries == 16 * displacements.size() &&
              core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Long branches cache by capture while reading current flags and PC without I/O");
}

void extended_jumps() {
  Core core;
  constexpr std::array<std::uint16_t, 9> targets{
      0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xfffd, 0xfffe, 0xffff};
  constexpr Byte opcode = 6;
  for (const auto target : targets) {
    const std::array<Byte, 3> code{
        opcode, static_cast<Byte>(target >> byte_bits), static_cast<Byte>(target)};
    for (const auto seed : seeds) {
      for (unsigned flags = 0; flags < flags_count; ++flags) {
        auto before = seed;
        before.ccr = static_cast<Byte>(flags);
        auto wanted = before;
        wanted.pc = target;
        retired(core, code, before, wanted);
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "An extended JMP does not read its target as a data operand");
}

void indexed_jumps() {
  std::array<Byte, address_bytes> ram{};
  Core core(Memory::unified(ram));
  constexpr Byte opcode = 5;
  constexpr std::uint16_t target = 0xa55a;
  for (const auto form : forms()) {
    const auto capture = encode(form, opcode);
    for (const auto seed : seeds) {
      const auto address = pointer(form, seed);
      ram[address] = static_cast<Byte>(target >> byte_bits);
      ram[static_cast<std::uint16_t>(static_cast<unsigned>(address) + 1U)] =
          static_cast<Byte>(target);
      for (unsigned flags = 0; flags < flags_count; ++flags) {
        auto before = seed;
        before.ccr = static_cast<Byte>(flags);
        auto wanted = before;
        wanted.pc = form.indirect ? target : address;
        retired(core, capture.view(), before, wanted);
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "JMP reads only an indirect pointer, directly from known RAM");
}

struct Auto {
  unsigned base;
  int delta;
  bool pre;
};

void auto_case(Core &core, const Auto point) {
  constexpr std::array<Field, 3> fields{&Registers::x, &Registers::y, &Registers::sp};
  constexpr Byte opcode = 5;
  constexpr unsigned auto_mode = 0x20;
  constexpr unsigned post_mode = 0x10;
  const auto raw = point.delta > 0 ? static_cast<unsigned>(point.delta - 1)
                                   : 16U - static_cast<unsigned>(-point.delta);
  const std::array<Byte, 2> code{
      opcode,
      static_cast<Byte>((point.base << 6U) | auto_mode | (point.pre ? 0U : post_mode) | raw)};
  for (const auto seed : seeds) {
    for (unsigned flags = 0; flags < flags_count; ++flags) {
      auto before = seed;
      before.ccr = static_cast<Byte>(flags);
      auto wanted = before;
      wanted.*fields[point.base] =
          static_cast<std::uint16_t>(static_cast<int>(before.*fields[point.base]) + point.delta);
      wanted.pc = point.pre ? wanted.*fields[point.base] : before.*fields[point.base];
      retired(core, code, before, wanted);
    }
  }
}

void auto_jumps() {
  Core core;
  for (unsigned base = 0; base < 3; ++base) {
    for (int delta = -8; delta <= 8; ++delta) {
      if (delta == 0) { continue; }
      for (const bool pre : {false, true}) {
        auto_case(core, {.base = base, .delta = delta, .pre = pre});
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Auto JMP adjusts the selected index without target data access or flag changes");
}

void dynamic_jumps() {
  Core core;
  constexpr Byte opcode = 5;
  constexpr std::array<Byte, 4> postbytes{0, 15, 16, 31};
  for (const auto postbyte : postbytes) {
    const int delta = postbyte < 16 ? postbyte : static_cast<int>(postbyte) - 32;
    const std::array<Byte, 2> code{opcode, postbyte};
    for (unsigned value = 0; value < address_bytes; ++value) {
      auto before = seeds[0];
      before.x = static_cast<std::uint16_t>(value);
      before.ccr = static_cast<Byte>(value);
      auto wanted = before;
      wanted.pc = static_cast<std::uint16_t>(static_cast<int>(value) + delta);
      retired(core, code, before, wanted);
    }
  }
  require(core.statistics().compiled_entries == postbytes.size(),
          "One native JMP capture reads every dynamic base value without recompilation");
}

struct Port {
  Core *core = nullptr;
  Registers before{};
  unsigned calls = 0;
  std::uint16_t address = 0, target = 0;
  bool accept = false, observed = true;

  static bool read(void *owner, const std::uint16_t address, std::uint16_t &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    ++port.calls;
    port.observed =
        port.observed && address == port.address && port.core->registers() == port.before;
    value = port.target;
    return port.accept;
  }
};

void indirect_faults() {
  constexpr std::uint16_t pointer_address = 0xffff;
  constexpr std::uint16_t initial_target = 0x4321;
  Port port{.before = seeds[0], .address = pointer_address, .target = initial_target};
  Io io{.owner = &port, .read_word = Port::read};
  Core core(Memory::external(), &io);
  port.core = &core;
  core.set_registers(port.before);
  constexpr std::array<Byte, 4> code{5, 0xe3, 0xff, 0xff}; // JMP [$ffff,X], X=0.
  const auto fault = core.run_instruction(code);
  require(fault.fault == Fault::memory_read && fault.retired == 0 && fault.pc == port.before.pc &&
              fault.opcode == code[0] && fault.address == port.address && fault.access_size == 2 &&
              core.registers() == port.before && port.calls == 1 && port.observed,
          "Indirect JMP refuses at its actual logical pointer access, without changing PC");
  core.clear_code_cache();
  require(core.run_instruction(code).retired == 0 && port.calls == 1,
          "A latched indirect JMP fault cannot repeat its pointer access");
  port.accept = true;
  auto wanted = port.before;
  wanted.pc = port.target;
  retired(core, code, port.before, wanted);
  port.target = 0;
  wanted.pc = port.target;
  retired(core, code, port.before, wanted);
  require(port.calls == 3 && port.observed && core.statistics().read_callbacks == 3 &&
              core.statistics().write_callbacks == 0,
          "Cached indirect JMP reads the current pointer and never reads the destination as data");
}

void refusals() {
  Core core;
  constexpr std::array<Byte, 4> valid{0x18, 0x20, 0, 0};
  auto wanted = seeds[0];
  wanted.pc = 2;
  retired(core, valid, seeds[0], wanted);
  const auto before = core.registers();
  const auto count = core.statistics();
  for (std::size_t length = 1; length < valid.size(); ++length) {
    require(core.run_instruction(std::span(valid).first(length)).fault ==
                    Fault::unsupported_instruction &&
                core.registers() == before,
            "A cached long branch cannot admit its truncated prefix");
  }
  constexpr std::array<std::array<Byte, 4>, 3> invalid{
      {{0x18, 0x19, 0, 0}, {0x18, 0x30, 0, 0}, {6, 0, 0, 0}}};
  for (const auto code : invalid) {
    require(core.run_instruction(code).fault == Fault::unsupported_instruction &&
                core.registers() == before,
            "Unsupported page-two instructions and extended-JMP padding refuse without effects");
  }
  require(core.statistics().compiled_entries == count.compiled_entries &&
              core.statistics().native_instructions == count.native_instructions,
          "Invalid control captures never compile or retire");
}
}

int main() {
  try {
    long_branches();
    extended_jumps();
    indexed_jumps();
    auto_jumps();
    dynamic_jumps();
    indirect_faults();
    refusals();
    std::cout << "Verified " << checked
              << " native long-branch/JMP cases, all CCR values, dynamic addresses, "
                 "indirect pointer faults and no target data access.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
