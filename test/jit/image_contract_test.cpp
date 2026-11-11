// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_support.hpp"
#include "isa.hpp"
#include "jit/image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace cpu12::test::image {
namespace {
constexpr std::uint64_t id = 7;
constexpr Byte sample = 0x80;

template <typename Action> void rejects(Action action, const std::string_view reason) {
  bool rejected = false;
  try {
    action();
  } catch (const std::invalid_argument &error) {
    rejected = std::string_view(error.what()).contains(reason);
  }
  require(rejected, "Malformed image/binding must fail with its actual context and reason");
}

void source_contract() {
  const Source source(sample);
  const auto valid = source.variant(id);
  const std::array variants{valid};
  const std::array duplicate{valid, valid};
  rejects([] { static_cast<void>(detail::ImagePlan({})); }, "no variants");
  rejects([&] { static_cast<void>(detail::ImagePlan({.variants = duplicate})); },
          "duplicate variant");
  auto missing = valid;
  missing.entries = {};
  const std::array missing_entries{missing};
  rejects([&] { static_cast<void>(detail::ImagePlan({.variants = missing_entries})); },
          "entries must be nonempty");
  const std::array<Address, 1> data_entry{table};
  missing = valid;
  missing.entries = data_entry;
  const std::array invalid_entry{missing};
  rejects([&] { static_cast<void>(detail::ImagePlan({.variants = invalid_entry})); }, "PC=16387");
  std::vector spans(source.spans.begin(), source.spans.end());
  spans.push_back(source.spans[0]);
  const std::array same{CodeVariant{.id = id, .code = spans, .entries = source.entries}};
  require(detail::ImagePlan({.variants = same}).variants()[0].instructions.size() == 10,
          "Identical overlapping source streams are deduplicated without losing valid entries");
  constexpr std::array<Byte, 1> conflicting{0x87};
  spans.back() = {.address = entry, .words = conflicting};
  rejects([&] { static_cast<void>(detail::ImagePlan({.variants = same})); },
          "overlapping executable spans disagree");
  require(detail::ImagePlan({.variants = variants}).variants()[0].instructions.size() == 10,
          "Rejected descriptions cannot mutate independently retained valid source declarations");
}

void instruction_contract() {
  constexpr std::array<Address, 1> entries{entry};
  constexpr std::array<Byte, 2> truncated{0x16, 0x41};
  constexpr std::array<Byte, 2> invalid{0x18, 0x18};
  constexpr std::array<Byte, 3> mid_instruction{6, 0x40, 1};
  constexpr std::array<Byte, 3> missing_call{0x16, 0x41, 0};
  constexpr std::array<Byte, 1> missing_fallthrough{0xa7};
  for (const auto words : {std::span<const Byte>(truncated),
                           std::span<const Byte>(invalid),
                           std::span<const Byte>(mid_instruction),
                           std::span<const Byte>(missing_call),
                           std::span<const Byte>(missing_fallthrough)}) {
    const std::array spans{CodeSpan{.address = entry, .words = words}};
    const std::array variants{CodeVariant{.id = id, .code = spans, .entries = entries}};
    rejects([&] { static_cast<void>(detail::ImagePlan({.variants = variants})); },
            "variant=7 PC=16384");
  }
  constexpr auto valid_wait = isa::program(isa::wai, std::array<Byte, 2>{bra, backedge});
  const std::array spans{CodeSpan{.address = entry, .words = valid_wait}};
  const std::array variants{CodeVariant{.id = id, .code = spans, .entries = entries}};
  const std::array layouts{VariantLayout{.id = id, .memory = layout}};
  const ImageProgram wait_image({.variants = variants}, layouts);
  require(wait_image.report().prepared_entries == 2 && wait_image.report().native_bytes != 0,
          "A complete declared WAI continuation receives prepared native code");
  constexpr auto valid_swi = isa::program(isa::swi, std::array<Byte, 2>{bra, backedge});
  const std::array unsupported_spans{CodeSpan{.address = entry, .words = valid_swi}};
  const std::array unsupported{
      CodeVariant{.id = id, .code = unsupported_spans, .entries = entries}};
  require(detail::ImagePlan({.variants = unsupported}).variants()[0].instructions.size() == 2,
          "Inventory distinguishes documented ISA from current LLVM lowering coverage");
  rejects([&] { static_cast<void>(ImageProgram({.variants = unsupported}, layouts)); },
          "no LLVM lowering");
}

void memory_contract() {
  const Source source(sample);
  const std::array variants{source.variant(id)};
  const std::array layouts{VariantLayout{.id = id, .memory = layout}};
  rejects([&] { static_cast<void>(ImageProgram({.variants = variants}, {})); },
          "one memory layout");
  const std::array duplicate{layouts[0], layouts[0]};
  rejects([&] { static_cast<void>(ImageProgram({.variants = variants}, duplicate)); },
          "duplicate variant memory");
  for (const unsigned option : {0U, 1U, 2U, 3U}) {
    auto malformed = layout;
    if (option == 0) { malformed[1].writable = true; }
    if (option == 1) { malformed[1].tracked = true; }
    if (option == 2) { malformed[1].size = address_bytes; }
    if (option == 3) { malformed[1].first = 0; }
    const std::array bad{VariantLayout{.id = id, .memory = malformed}};
    constexpr std::array<std::string_view, 4> reasons{
        "immutable binding view", "immutable binding view", "outside", "overlap"};
    rejects([&] { static_cast<void>(ImageProgram({.variants = variants}, bad)); }, reasons[option]);
  }
}
}

void rejected_images() {
  source_contract();
  instruction_contract();
  memory_contract();
}

void binding_contract() {
  const Source source(sample);
  const std::array variants{source.variant(id)};
  const std::array layouts{VariantLayout{.id = id, .memory = layout}};
  const ImageProgram image({.variants = variants}, layouts);
  Fixture fixture(source);
  image.validate_binding(id, fixture.memory());
  rejects([&] { image.validate_binding(id + 1, fixture.memory()); }, "Unknown CPU12 image variant");
  rejects([&] { static_cast<void>(image.entry(id, entry + 1)); }, "undeclared instruction entry");
  const auto original = fixture.bytes;
  fixture.bytes[entry + 1] ^= 1;
  rejects([&] { image.validate_binding(id, fixture.memory()); }, "PC=16385");
  fixture.bytes = original;
  auto changed = fixture.grants;
  changed[1].writable = true;
  rejects([&] { image.validate_binding(id, Memory::mapped(changed)); }, "memory layout");
  changed = fixture.grants;
  changed[0].bytes = std::span(fixture.bytes).subspan(entry, entry);
  rejects([&] { image.validate_binding(id, Memory::mapped(changed)); },
          "executable source does not match");
  image.validate_binding(id, fixture.memory());
  Owner owner(image, id, fixture.memory());
  owner.seed({.pc = cases[0] + 2});
  constexpr unsigned grant = 17;
  require(owner.run(grant).retired == grant &&
              owner.context.progress.estimated_cycles == std::uint64_t{grant} * 3,
          "Rejected bindings do not alter retained valid native code or owner state");
}

void alternate_streams() {
  constexpr std::array<Byte, 4> bytes{0x86, 0x87, bra, 0xfe};
  const std::array spans{CodeSpan{.address = entry, .words = bytes},
                         CodeSpan{.address = entry + 1, .words = std::span(bytes).subspan(1)}};
  constexpr std::array<Address, 2> entries{entry, entry + 1};
  const std::array variants{CodeVariant{.id = id, .code = spans, .entries = entries}};
  const std::array layouts{VariantLayout{.id = id, .memory = layout}};
  const ImageProgram image({.variants = variants}, layouts);
  require(
      image.instructions(id).size() == 3,
      "Consistent declared overlapping interpretations each receive a native instruction entry");
  std::array<Byte, address_bytes> memory{};
  std::ranges::copy(bytes, memory.begin() + entry);
  const std::array grants{Ram{.bytes = std::span(memory).first(entry)},
                          Ram{.first = entry,
                              .bytes = std::span(memory).subspan(entry, code_end - entry),
                              .writable = false},
                          Ram{.first = code_end, .bytes = std::span(memory).subspan(code_end)}};
  Owner owner(image, id, Memory::mapped(grants));
  constexpr Registers seed{
      .pc = entry + 1, .d = 0xabcd, .x = 0x1234, .y = 0x5678, .sp = 0x9000, .ccr = 0x80};
  auto wanted = seed;
  wanted.pc = entry + 2;
  wanted.d &= byte_mask;
  wanted.ccr |= 4;
  owner.seed(seed);
  require(owner.run(1).retired == 1 &&
              owner.context.cpu == State{.registers = wanted, .failure = {}} &&
              owner.context.progress == Progress{.estimated_cycles = 1},
          "An explicit alternate entry executes its own original instruction, not an enclosing "
          "interpretation");
}

void wrapping_stream() {
  constexpr Address first = 0xffff;
  constexpr std::array<Byte, 4> bytes{0x86, 0x80, bra, backedge};
  constexpr std::array<Address, 1> entries{first};
  const std::array spans{CodeSpan{.address = first, .words = bytes}};
  const std::array variants{CodeVariant{.id = id, .code = spans, .entries = entries}};
  constexpr std::array full_layout{
      RegionLayout{.first = 0, .size = address_bytes, .writable = false}};
  const std::array layouts{VariantLayout{.id = id, .memory = full_layout}};
  const ImageProgram image({.variants = variants}, layouts);
  std::array<Byte, address_bytes> memory{};
  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    memory[static_cast<Address>(first + offset)] = bytes[offset];
  }
  const auto before = memory;
  const std::array grants{Ram{.bytes = memory, .writable = false}};
  Owner owner(image, id, Memory::mapped(grants));
  constexpr Registers seed{.pc = first, .d = 0xabcd, .ccr = 0x43};
  constexpr Registers wanted{.pc = 1, .d = 0x80cd, .ccr = 0x49};
  owner.seed(seed);
  require(image.instructions(id).size() == 2 && owner.run(1).retired == 1 &&
              owner.context.cpu == State{.registers = wanted, .failure = {}} &&
              owner.context.progress == Progress{.estimated_cycles = 1},
          "Executable source spans preserve a wrapping multi-byte instruction and successor");
  constexpr unsigned grant = 17;
  require(owner.run(grant).retired == grant &&
              owner.context.cpu == State{.registers = wanted, .failure = {}} &&
              owner.context.progress ==
                  Progress{.estimated_cycles = 1 + (std::uint64_t{grant} * 3)} &&
              memory == before,
          "Wrapped native entries retain exact code bytes and independent progress");
}
}
