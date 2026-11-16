// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction_image.hpp"
#include "isa.hpp"
#include "stack_model.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <vector>

using cpu12::test::InstructionCore;

namespace {
using namespace cpu12;
using namespace cpu12::test::stack;
constexpr Address entry = 0x4000;
constexpr std::array<std::uint16_t, 6> positions{0, 1, 2, 0x7fff, 0xfffe, 0xffff};
constexpr std::array<std::uint16_t, 8> operands{0, 1, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff};

// The immutable code window splits the data grants. A word spanning FFFF/0000
// is one real owner transaction, not two independent byte callbacks.
struct WrappedRam {
  static constexpr Address last = 0xffff;
  std::span<Byte> bytes;

  static bool read(void *owner, const Address address, Address &value) noexcept {
    if (address != last) { return false; }
    const auto &ram = *static_cast<WrappedRam *>(owner);
    value = word(ram.bytes, address);
    return true;
  }

  static bool write(void *owner, const WordWrite access) noexcept {
    if (access.address != last) { return false; }
    const auto &ram = *static_cast<WrappedRam *>(owner);
    word(ram.bytes, access.address, access.value);
    return true;
  }
};

struct Input {
  StackOp operation;
  Registers before;
  std::uint16_t operand;
};

void stack_case(InstructionCore &core, const std::span<Byte> ram, const Input input) {
  const auto operation = input.operation;
  // Numerical stack cases keep every data/register input but separate real code
  // from the varied stack bytes formerly underneath captured instructions.
  auto before = input.before;
  before.pc = entry;
  const auto wanted = expected(operation, before, input.operand);
  const auto address = operation.push ? wanted.sp : before.sp;
  const auto left = static_cast<std::uint16_t>(address + 0xffffU);
  const auto right = static_cast<std::uint16_t>(static_cast<unsigned>(address) + operation.width);
  constexpr Byte left_guard = 0xa5;
  constexpr Byte right_guard = 0x5a;
  ram[left] = left_guard;
  ram[right] = right_guard;
  if (operation.width == 2) {
    word(ram, address, input.operand);
  } else {
    ram[address] = static_cast<Byte>(input.operand);
  }
  core.select(std::array{operation.opcode}, before.pc);
  retired(core, std::array{operation.opcode}, before, wanted);
  const auto actual = operation.width == 2 ? word(ram, address) : ram[address];
  require(actual == (operation.push ? value(before, operation.target) : input.operand) &&
              ram[left] == left_guard && ram[right] == right_guard,
          "Stack access uses only its selected bytes, with independent big-endian data");
}

void inherent_matrix(InstructionCore &core, const std::span<Byte> ram) {
  for (const auto operation : operations) {
    for (const auto seed : seeds) {
      for (const auto position : positions) {
        for (const auto operand : operands) {
          auto before = seed;
          before.sp = position;
          const auto data =
              static_cast<std::uint16_t>(operation.width == 2 ? operand : operand & byte_mask);
          stack_case(core, ram, {.operation = operation, .before = before, .operand = data});
        }
      }
    }
  }
}

void full_word_domains(InstructionCore &core, const std::span<Byte> ram) {
  for (const auto operation : operations) {
    if (operation.width != 2) { continue; }
    for (unsigned operand = 0; operand < memory_bytes; ++operand) {
      auto before = seeds[0];
      before.sp = operation.push ? 1 : static_cast<std::uint16_t>(memory_bytes - 1);
      before.ccr = static_cast<Byte>(operand);
      const auto data = static_cast<std::uint16_t>(operand);
      if (operation.push) { assign(before, operation.target, data); }
      stack_case(core, ram, {.operation = operation, .before = before, .operand = data});
    }
  }
}

void full_ccr_domains(InstructionCore &core, const std::span<Byte> ram) {
  constexpr StackOp pulc{.opcode = static_cast<Byte>(isa::Opcode::pulc),
                         .target = Target::ccr,
                         .width = 1,
                         .push = false};
  for (const auto position : positions) {
    for (unsigned old = 0; old <= byte_mask; ++old) {
      auto before = seeds[0];
      before.sp = position;
      before.ccr = static_cast<Byte>(old);
      for (unsigned operand = 0; operand <= byte_mask; ++operand) {
        stack_case(
            core,
            ram,
            {.operation = pulc, .before = before, .operand = static_cast<std::uint16_t>(operand)});
      }
    }
  }
}

void full_byte_domains(InstructionCore &core, const std::span<Byte> ram) {
  for (const auto operation : operations) {
    if (operation.width != 1) { continue; }
    for (unsigned operand = 0; operand <= byte_mask; ++operand) {
      auto before = seeds[0];
      const auto data = static_cast<std::uint16_t>(operand);
      if (operation.push) { assign(before, operation.target, data); }
      stack_case(core, ram, {.operation = operation, .before = before, .operand = data});
    }
  }
}

void nested_program() {
  std::array<Byte, memory_bytes> ram{};
  constexpr std::uint16_t main_entry = 0x8000;
  constexpr std::uint16_t routine = 0x8100;
  constexpr std::uint16_t inner = 0x8110;
  constexpr auto invoke = isa::program(isa::opcode<isa::Opcode::jsr_extended>(),
                                       std::array<Byte, 2>{0x81, 0},
                                       isa::opcode<isa::Opcode::bra_relative>(),
                                       std::array<Byte, 1>{0xfe});
  constexpr auto body = isa::program(isa::pshd,
                                     isa::opcode<isa::Opcode::ldd_immediate>(),
                                     std::array<Byte, 2>{0x12, 0x34},
                                     isa::opcode<isa::Opcode::bsr_relative>(),
                                     std::array<Byte, 1>{10},
                                     isa::puld,
                                     isa::rts);
  constexpr auto nested = isa::program(
      isa::opcode<isa::Opcode::ldx_immediate>(), std::array<Byte, 2>{0xab, 0xcd}, isa::rts);
  for (unsigned i = 0; i < invoke.size(); ++i) { ram[main_entry + i] = invoke[i]; }
  for (unsigned i = 0; i < body.size(); ++i) { ram[routine + i] = body[i]; }
  for (unsigned i = 0; i < nested.size(); ++i) { ram[inner + i] = nested[i]; }
  const std::array spans{CodeSpan{.address = main_entry, .words = invoke},
                         CodeSpan{.address = routine, .words = body},
                         CodeSpan{.address = inner, .words = nested}};
  constexpr std::array entries{main_entry};
  const std::array variants{CodeVariant{.id = 0, .code = spans, .entries = entries}};
  const auto image = Core::compile({.variants = variants},
                                   {.memory = {},
                                    .interrupts = {},
                                    .transitions = {},
                                    .callbacks = {},
                                    .bindings = {},
                                    .topology = MemoryTopology::dynamic});
  std::vector<Ram> grants;
  grants.reserve((spans.size() * 2) + 1);
  std::size_t cursor = 0;
  for (const auto span : spans) {
    grants.push_back({.first = static_cast<Address>(cursor),
                      .bytes = std::span(ram).subspan(cursor, span.address - cursor)});
    grants.push_back({.first = span.address,
                      .bytes = std::span(ram).subspan(span.address, span.words.size()),
                      .writable = false});
    cursor = span.address + span.words.size();
  }
  grants.push_back(
      {.first = static_cast<Address>(cursor), .bytes = std::span(ram).subspan(cursor)});
  WrappedRam storage{.bytes = ram};
  Io io{.owner = &storage, .read_word = WrappedRam::read, .write_word = WrappedRam::write};
  Core core(Memory::mapped(grants), &io);
  core.install(image, 0);
  constexpr Registers initial{.pc = main_entry, .d = 0xbeef, .x = 0, .y = 0, .sp = 1, .ccr = 0xe1};
  constexpr std::array<unsigned, 8> lengths{3, 1, 3, 2, 3, 1, 1, 1};
  constexpr std::array<Registers, 8> wanted{
      {{.pc = 0x8100, .d = 0xbeef, .x = 0, .y = 0, .sp = 0xffff, .ccr = 0xe1},
       {.pc = 0x8101, .d = 0xbeef, .x = 0, .y = 0, .sp = 0xfffd, .ccr = 0xe1},
       {.pc = 0x8104, .d = 0x1234, .x = 0, .y = 0, .sp = 0xfffd, .ccr = 0xe1},
       {.pc = 0x8110, .d = 0x1234, .x = 0, .y = 0, .sp = 0xfffb, .ccr = 0xe1},
       {.pc = 0x8113, .d = 0x1234, .x = 0xabcd, .y = 0, .sp = 0xfffb, .ccr = 0xe9},
       {.pc = 0x8106, .d = 0x1234, .x = 0xabcd, .y = 0, .sp = 0xfffd, .ccr = 0xe9},
       {.pc = 0x8107, .d = 0xbeef, .x = 0xabcd, .y = 0, .sp = 0xffff, .ccr = 0xe9},
       {.pc = 0x8003, .d = 0xbeef, .x = 0xabcd, .y = 0, .sp = 1, .ccr = 0xe9}}};
  core.set_registers(initial);
  for (unsigned i = 0; i < lengths.size(); ++i) {
    const auto before = core.registers();
    retired(core, std::span(ram).subspan(before.pc, lengths[i]), before, wanted[i]);
  }
  constexpr std::uint16_t outer_address = 0xffff;
  constexpr std::uint16_t outer_return = 0x8003;
  constexpr std::uint16_t saved_address = 0xfffd;
  constexpr std::uint16_t saved_d = 0xbeef;
  constexpr std::uint16_t inner_address = 0xfffb;
  constexpr std::uint16_t inner_return = 0x8106;
  require(
      word(ram, outer_address) == outer_return && word(ram, saved_address) == saved_d &&
          word(ram, inner_address) == inner_return,
      "Authored nested calls execute the expected original instructions and actual stack stores");
}

}

int main() {
  try {
    std::array<Byte, memory_bytes> ram{};
    std::vector<std::vector<Byte>> programs;
    programs.reserve(operations.size());
    for (const auto operation : operations) { programs.push_back({operation.opcode}); }
    constexpr std::array entries{entry};
    constexpr Address code_end = entry + 3;
    const std::array data{Ram{.bytes = std::span(ram).first(entry)},
                          Ram{.first = code_end, .bytes = std::span(ram).subspan(code_end)}};
    WrappedRam storage{.bytes = ram};
    Io io{.owner = &storage, .read_word = WrappedRam::read, .write_word = WrappedRam::write};
    InstructionCore core(programs, entries, &io, {}, data);
    inherent_matrix(core, ram);
    full_word_domains(core, ram);
    full_byte_domains(core, ram);
    full_ccr_domains(core, ram);
    nested_program();
    const auto contexts = external_bus();
    std::cout << "Verified " << checked
              << " native stack/call cases, CCR X restrictions, nested returns, "
                 "wrapped stack data and complete independent register/memory expectations; "
              << contexts << " external access contexts.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
