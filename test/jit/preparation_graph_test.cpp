// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "instruction.hpp"
#include "isa.hpp"
#include "jit/preparation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#endif

namespace {
using namespace cpu12;
using detail::Kind;
constexpr Address entry = 0x4000;
constexpr Byte padding = 0x18;
constexpr Address immediate = 0x1234;
constexpr Byte immediate_high = static_cast<Byte>(immediate >> 8);
constexpr Byte immediate_low = static_cast<Byte>(immediate);
constexpr Address helper_entry = 32;
constexpr Address helper_return = 35;

void require(const bool accepted, const char *message) {
  if (!accepted) { throw std::runtime_error(message); }
}

template <isa::Opcode Opcode> constexpr Byte opcode = static_cast<Byte>(Opcode);

void reachable_graph() {
  std::array<Byte, 36> bytes;
  bytes.fill(padding);
  const auto put = [&](std::size_t offset, std::span<const Byte> code) {
    std::ranges::copy(code, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
  };
  constexpr std::array branch{opcode<isa::Opcode::bra_relative>, Byte{4}};
  constexpr std::array loop{opcode<isa::Opcode::subd_immediate>,
                            Byte{0},
                            Byte{1},
                            opcode<isa::Opcode::bne_relative>,
                            Byte{0xfb}};
  constexpr std::array call{
      opcode<isa::Opcode::jsr_extended>, Byte{0x40}, Byte{0x20}, opcode<isa::Opcode::rts>};
  constexpr std::array helper{
      opcode<isa::Opcode::ldd_immediate>, Byte{0xbe}, Byte{0xef}, opcode<isa::Opcode::rts>};
  put(0, branch);
  put(6, loop);
  put(11, call);
  put(helper_entry, helper);
  const auto before = bytes;
  const auto memory = Memory::window({.first = entry, .bytes = bytes, .writable = false});
  constexpr std::array<Address, 2> entries{entry, entry};
  const auto graph = detail::preparation_graph(memory, entries);
  constexpr std::array<Address, 7> offsets{0, 6, 9, 11, 14, 32, 35};
  require(graph.failure.fault == Fault::none && graph.instructions.size() == offsets.size(),
          "Direct reachability must join loop/call targets and ignore duplicate seeds and padding");
  for (std::size_t index = 0; index < offsets.size(); ++index) {
    require(graph.instructions[index].pc == entry + offsets[index],
            "Preparation must discover only established direct successors");
  }
  require(graph.unresolved == std::vector<Address>{entry + 14, entry + helper_return} &&
              bytes == before,
          "Dynamic returns remain unresolved and preparation preserves original program bytes");
  const auto empty = detail::preparation_graph(memory, {});
  require(empty.instructions.empty() && empty.unresolved.empty() &&
              empty.failure.fault == Fault::none,
          "An empty seed set never scans code or data");
}

void conditional_edges() {
  constexpr Address wrapped = 0xffff;
  constexpr Byte condition_count = 16;
  for (Byte condition = 0; condition < condition_count; ++condition) {
    const std::array code{static_cast<Byte>(opcode<isa::Opcode::bra_relative> + condition),
                          Byte{0xfd}};
    const auto instruction = detail::decode_first(code);
    if (!instruction) { throw std::runtime_error("Every authored short branch must decode"); }
    const auto flow = detail::successors({.instruction = instruction.value(), .pc = wrapped});
    constexpr Address next = 1;
    constexpr Address target = 0xfffe;
    require(!flow.indirect && flow.count == (condition < 2 ? 1U : 2U),
            "Always, never and conditional branches have exact static edge counts");
    if (condition == 0) {
      require(flow.direct[0] == target, "BRA has no invented fallthrough");
    } else {
      require(flow.direct[0] == next && (condition == 1 || flow.direct[1] == target),
              "BRN has no invented target; conditional targets use signed logical wrap");
    }
  }
}

void indirect_edges() {
  constexpr std::array jump{opcode<isa::Opcode::jmp_indexed>, Byte{0}};
  constexpr std::array call{opcode<isa::Opcode::jsr_indexed>, Byte{0}};
  const auto jump_instruction = detail::decode_first(jump);
  const auto call_instruction = detail::decode_first(call);
  if (!jump_instruction || !call_instruction) {
    throw std::runtime_error("Authored indexed control transfers must decode");
  }
  const auto jump_flow = detail::successors({.instruction = jump_instruction.value(), .pc = entry});
  const auto call_flow = detail::successors({.instruction = call_instruction.value(), .pc = entry});
  require(jump_flow.count == 0 && jump_flow.indirect && call_flow.count == 1 &&
              call_flow.indirect && call_flow.direct[0] == entry + call.size(),
          "Unknown indexed transfers retain their unresolved edge and only a real call return PC");
}

void static_call_edges() {
  constexpr Byte short_displacement = 0xfd;
  constexpr std::array relative{opcode<isa::Opcode::bsr_relative>, short_displacement};
  constexpr std::array direct{opcode<isa::Opcode::jsr_direct>, immediate_low};
  constexpr std::array extended{opcode<isa::Opcode::jmp_extended>, immediate_high, immediate_low};
  constexpr std::array long_branch{
      Byte{0x18}, opcode<isa::Opcode::bra_relative>, Byte{0xff}, Byte{0xfb}};
  const auto inspect = [](std::span<const Byte> code) {
    const auto instruction = detail::decode_first(code);
    if (!instruction) { throw std::runtime_error("Authored static control transfer must decode"); }
    return detail::successors({.instruction = instruction.value(), .pc = entry});
  };
  const auto relative_flow = inspect(relative);
  const auto direct_flow = inspect(direct);
  const auto extended_flow = inspect(extended);
  const auto long_flow = inspect(long_branch);
  require(relative_flow.count == 2 && !relative_flow.indirect &&
              relative_flow.direct[0] == entry + relative.size() &&
              relative_flow.direct[1] == entry - 1 && direct_flow.count == 2 &&
              !direct_flow.indirect && direct_flow.direct[1] == immediate_low &&
              extended_flow.count == 1 && !extended_flow.indirect &&
              extended_flow.direct[0] == immediate && long_flow.count == 1 && !long_flow.indirect &&
              long_flow.direct[0] == entry - 1,
          "Relative/direct calls, absolute jumps and long branches retain exact signed targets");
}

void external_continuations() {
  for (const auto code : {std::span<const Byte>(isa::stop),
                          std::span<const Byte>(isa::wai),
                          std::span<const Byte>(isa::swi)}) {
    const auto instruction = detail::decode_first(code);
    if (!instruction) { throw std::runtime_error("Authored external transfer must decode"); }
    const auto flow = detail::successors({.instruction = instruction.value(), .pc = entry});
    require(flow.indirect && flow.count == 1 && flow.direct[0] == entry + code.size(),
            "STOP fallthrough and WAI/SWI return sites retain their known continuation and "
            "unresolved handler");
  }
}

void unavailable_code() {
  auto bytes = isa::program(isa::opcode<isa::Opcode::ldd_immediate>(),
                            std::array<Byte, 2>{immediate_high, immediate_low});
  const auto before = bytes;
  constexpr std::array<Address, 1> entries{entry};
  const auto writable = Memory::window({.first = entry, .bytes = bytes});
  const auto graph = detail::preparation_graph(writable, entries);
  require(graph.instructions.empty() && graph.unresolved == std::vector<Address>{entry} &&
              graph.failure.fault == Fault::none,
          "Writable code remains unresolved without executing a guest instruction");
  std::array<Byte, bytes.size()> known;
  known.fill(1);
  const auto live_knownness =
      Memory::window({.first = entry, .bytes = bytes, .known = known, .writable = false});
  require(detail::preparation_graph(live_knownness, entries).instructions.empty(),
          "Live knownness is not an immutable instruction contract");
  const auto prefix =
      Memory::window({.first = entry, .bytes = std::span(bytes).first(2), .writable = false});
  const auto capture = detail::fetch_immutable(prefix, entry);
  require(!capture.instruction && capture.failure.fault == Fault::instruction_fetch &&
              capture.failure.pc == entry && capture.failure.opcode == bytes[0] &&
              capture.failure.address == entry + 2 && capture.failure.retired == 0 &&
              capture.failure.access_size == 1 &&
              capture.failure.read_mask == std::numeric_limits<Byte>::max(),
          "Unavailable immutable operands retain the precise non-retiring fetch diagnostic");
  require(bytes == before, "Rejected preparation leaves all source storage unchanged");
}

void split_and_aliases() {
  auto bytes = isa::program(isa::opcode<isa::Opcode::ldd_immediate>(),
                            std::array<Byte, 2>{immediate_high, immediate_low});
  constexpr Address start = 0xfffe;
  const std::array split{Ram{.first = start, .bytes = std::span(bytes).first(2), .writable = false},
                         Ram{.bytes = std::span(bytes).last(1), .writable = false}};
  const auto capture = detail::fetch_immutable(Memory::mapped(split), start);
  require(capture.instruction && capture.instruction->kind == Kind::load &&
              capture.instruction->operand == immediate && capture.failure.fault == Fault::none,
          "Immutable capture crosses exact grants and the 16-bit wrap");
  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    const std::array regions{Ram{.first = entry, .bytes = bytes, .writable = false},
                             Ram{.first = 0x8000, .bytes = std::span(bytes).subspan(offset, 1)}};
    const auto refused = detail::fetch_immutable(Memory::mapped(regions), entry);
    require(!refused.instruction && refused.failure.fault == Fault::instruction_fetch &&
                refused.failure.address == entry + offset,
            "Every writable physical alias is rejected before its corresponding byte is read");
  }
}

void guarded_writable_storage() {
#if __has_feature(address_sanitizer)
  alignas(8) std::array<Byte, 8> bytes{};
  const auto memory = Memory::window({.first = entry, .bytes = bytes});
  __asan_poison_memory_region(bytes.data(), bytes.size());
  const auto capture = detail::fetch_immutable(memory, entry);
  const std::array regions{Ram{.first = entry, .bytes = bytes, .writable = false},
                           Ram{.first = 0x8000, .bytes = bytes}};
  const auto alias = detail::fetch_immutable(Memory::mapped(regions), entry);
  __asan_unpoison_memory_region(bytes.data(), bytes.size());
  require(!capture.instruction && capture.failure.fault == Fault::instruction_fetch &&
              capture.failure.address == entry && !alias.instruction &&
              alias.failure.fault == Fault::instruction_fetch && alias.failure.address == entry,
          "Preparation never dereferences writable or physically aliased instruction storage");
#endif
}

void invalid_instruction() {
  std::array<Byte, 2> bytes{padding, padding};
  const auto memory = Memory::window({.first = entry, .bytes = bytes, .writable = false});
  constexpr std::array<Address, 1> entries{entry};
  const auto graph = detail::preparation_graph(memory, entries);
  const RunResult wanted{.fault = Fault::unsupported_instruction, .pc = entry, .opcode = padding};
  require(graph.instructions.empty() && graph.unresolved.empty() && graph.failure == wanted,
          "Established invalid code reports its original opcode/PC without retiring or inventing a "
          "successor");
}

void entire_address_space() {
  std::array<Byte, address_bytes> bytes;
  bytes.fill(isa::nop[0]);
  const auto memory = Memory::window({.bytes = bytes, .writable = false});
  constexpr std::array<Address, 1> entries{0};
  const auto graph = detail::preparation_graph(memory, entries);
  require(graph.instructions.size() == address_bytes && graph.unresolved.empty() &&
              graph.failure.fault == Fault::none,
          "The complete closed address space is discovered without a translation cap");
  for (std::size_t index = 0; index < graph.instructions.size(); ++index) {
    require(graph.instructions[index].pc == index &&
                graph.instructions[index].instruction.kind == Kind::nop,
            "Full-space discovery retains every original instruction location exactly once");
  }
}
}

int main() try {
  reachable_graph();
  conditional_edges();
  indirect_edges();
  static_call_edges();
  external_continuations();
  unavailable_code();
  split_and_aliases();
  guarded_writable_storage();
  invalid_instruction();
  entire_address_space();
  std::cout << "Immutable preparation, exact direct CFG, unresolved edges and full address space "
               "passed\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
