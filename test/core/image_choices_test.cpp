// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_transitions.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>

namespace cpu12::test::transitions {
namespace {
constexpr std::uint64_t alternative = 11, excluded = 13;
constexpr Byte first_value = 0x7f, second_value = 0x22, excluded_value = 0x33;
constexpr auto source =
    isa::program(isa::opcode<isa::Opcode::stab_extended>(),
                 std::array<Byte, 2>{static_cast<Byte>(destination >> byte_bits),
                                     static_cast<Byte>(destination + 1)},
                 jump(destination));
constexpr auto second = isa::program(
    isa::opcode<isa::Opcode::ldab_immediate>(), std::array<Byte, 1>{second_value}, loop);
constexpr auto third = isa::program(
    isa::opcode<isa::Opcode::ldab_immediate>(), std::array<Byte, 1>{excluded_value}, loop);
constexpr std::array choices{forward[0],
                             CodeTransition{.from = from, .to = alternative, .entry = destination}};

NativeImage prepare(const MemoryTopology topology) {
  const std::array caller{CodeSpan{.address = start, .words = source}};
  const std::array first_code{CodeSpan{.address = destination, .words = payload}};
  const std::array second_code{CodeSpan{.address = destination, .words = second}};
  const std::array third_code{CodeSpan{.address = destination, .words = third}};
  const std::array variants{CodeVariant{.id = from, .code = caller, .entries = roots},
                            CodeVariant{.id = to, .code = first_code, .entries = targets},
                            CodeVariant{.id = alternative, .code = second_code, .entries = targets},
                            CodeVariant{.id = excluded, .code = third_code, .entries = targets}};
  const auto original = layout(source, payload, false);
  const auto loaded = layout(source, payload, true);
  const std::array layouts{VariantLayout{.id = from, .memory = original},
                           VariantLayout{.id = to, .memory = loaded},
                           VariantLayout{.id = alternative, .memory = loaded},
                           VariantLayout{.id = excluded, .memory = loaded}};
  const auto memory = topology == MemoryTopology::fixed ? std::span<const VariantLayout>{layouts}
                                                        : std::span<const VariantLayout>{};
  return Core::compile({.variants = variants},
                       {.memory = memory,
                        .interrupts = {},
                        .transitions = choices,
                        .callbacks = {},
                        .bindings = {},
                        .topology = topology});
}

void bind_choice(Core &core, Fixture &fixture, const NativeImage &image, const Byte value) {
  const auto before = core.state();
  const auto progress = core.progress();
  const auto bytes = fixture.bytes;
  const auto known = fixture.known;
  const auto selected = value == first_value ? to : alternative;
  const auto mismatched = value == first_value ? alternative : to;
  core.set_memory(fixture.memory(true));
  core.set_memory(fixture.memory(true));
  rejects([&] { core.install(image, excluded); }, "declared image and variant");
  rejects([&] { core.install(image, from); }, "declared image and variant");
  rejects([&] { core.install(image, mismatched); }, "executable source does not match");
  require(core.run(8).retired == 0 && core.run_until(32).retired == 0 && core.state() == before &&
              core.progress() == progress && fixture.bytes == bytes && fixture.known == known,
          "Finite choices survive repeated remaps and rejected identities/bytes without replay");
  core.install(image, selected);
  require(core.state() == before && core.progress() == progress && fixture.bytes == bytes &&
              fixture.known == known && core.statistics().compiled_entries == 0,
          "A matching declared choice binds retained native code without changing guest state");
  auto expected = before;
  expected.registers.pc = destination + 2;
  require(core.step().retired == 1 && core.state() == expected &&
              core.progress().estimated_cycles == progress.estimated_cycles + 1 &&
              fixture.bytes == bytes && fixture.known == known,
          "The chosen original target executes its LDAB once with exact flags and storage");
}

struct Case {
  Byte value, flags;
  unsigned limit;
  bool timed;
};

unsigned prefix_length(const Case &choice) {
  if (!choice.timed) { return std::min(choice.limit, 2U); }
  if (choice.limit == 0) { return 0; }
  return choice.limit <= 3 ? 1U : 2U;
}

void prefix(const NativeImage &image, const Case &choice) {
  const auto value = choice.value;
  const auto flags = choice.flags;
  const auto limit = choice.limit;
  const auto timed = choice.timed;
  Fixture fixture(source, payload, true);
  Core core(fixture.memory());
  core.install(image, from);
  auto expected = seed;
  constexpr Address accumulator_a_mask = 0xff00;
  expected.d = static_cast<Address>((expected.d & accumulator_a_mask) | value);
  expected.ccr = flags;
  core.set_registers(expected);
  auto bytes = fixture.bytes;
  const auto known = fixture.known;
  const auto retired = prefix_length(choice);
  const auto result = timed ? core.run_until(limit) : core.run(limit);
  if (retired != 0) {
    bytes[destination + 1] = value;
    expected.ccr = static_cast<Byte>(flags & ~nzv);
    expected.pc = retired == 1 ? start + 3 : destination;
  }
  require(result.fault == Fault::none && result.retired == retired &&
              core.state() == State{.registers = expected, .failure = {}} &&
              core.progress().estimated_cycles == std::uint64_t{retired} * 3 &&
              fixture.bytes == bytes && fixture.known == known,
          "Native STAB/JMP prefixes match independent registers, flags, cycles and all storage");
  const auto remaining = core.run(8);
  bytes[destination + 1] = value;
  expected.ccr = static_cast<Byte>(flags & ~nzv);
  expected.pc = destination;
  require(remaining.retired == 2 - retired && remaining.fault == Fault::none &&
              core.state() == State{.registers = expected, .failure = {}} &&
              core.progress().estimated_cycles == 6 && fixture.bytes == bytes &&
              fixture.known == known,
          "The actual guest store selects its helper bytes before the real jump suspends");
  bind_choice(core, fixture, image, value);
}
}
}

int main(const int argc, const char *const *const argv) try {
  using namespace cpu12::test::transitions;
  const bool dynamic = argc == 2 && std::string_view(argv[1]) == "dynamic";
  require(argc == 1 || dynamic, "Expected no argument or dynamic topology");
  const auto image =
      prepare(dynamic ? cpu12::MemoryTopology::dynamic : cpu12::MemoryTopology::fixed);
  require(image.report().prepared_entries == 8 && image.report().unresolved_entries == 0,
          "Complete AOT retains the caller and all three real helper versions");
  for (unsigned flags = 0; flags < 256; ++flags) {
    for (const auto value : {first_value, second_value}) {
      for (unsigned limit = 0; limit <= 8; ++limit) {
        prefix(image,
               {.value = value,
                .flags = static_cast<cpu12::Byte>(flags),
                .limit = limit,
                .timed = false});
        prefix(image,
               {.value = value,
                .flags = static_cast<cpu12::Byte>(flags),
                .limit = limit,
                .timed = true});
      }
    }
  }
  std::cout << "Verified 9216 native source/choice prefixes, exact guest stores and retained "
               "multi-target admission across remaps.\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
