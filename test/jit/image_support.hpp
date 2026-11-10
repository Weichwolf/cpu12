// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "jit/block.hpp"
#include "jit/image.hpp"
#include "jit/llvm/image.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

namespace cpu12::test::image {
using detail::CodeImage;
using detail::CodeSpan;
using detail::CodeVariant;
using detail::llvm19::ImageProgram;
using detail::llvm19::RegionLayout;
using detail::llvm19::VariantLayout;
constexpr Address entry = 0x4000, table = entry + 3, helper = 0x4100, code_end = 0x4200;
constexpr unsigned byte_bits = 8, byte_mask = 0xff;
constexpr Byte padding = 0x18;
constexpr Byte bra = static_cast<Byte>(isa::Opcode::bra_relative), backedge = 0xfe;
constexpr std::array<Address, 3> cases{table + 0x18, table + 0x28, table + 0x30};
constexpr std::array<RegionLayout, 3> layout{
    RegionLayout{.first = 0, .size = entry},
    RegionLayout{.first = entry, .size = code_end - entry, .writable = false},
    RegionLayout{.first = code_end, .size = address_bytes - code_end}};
constexpr auto call_encoding = isa::program(
    isa::opcode<isa::Opcode::jsr_extended>(),
    std::array<Byte, 2>{static_cast<Byte>(helper >> byte_bits), static_cast<Byte>(helper)});
constexpr auto dispatch_encoding = isa::program(isa::opcode<isa::Opcode::pulx>(),
                                                isa::opcode<isa::Opcode::ldab_indexed>(),
                                                std::array<Byte, 1>{0xe5},
                                                isa::opcode<isa::Opcode::jmp_indexed>(),
                                                std::array<Byte, 1>{0xe5});

constexpr std::array<Byte, 4> body(const Byte value) {
  return {static_cast<Byte>(isa::Opcode::ldaa_immediate), value, bra, backedge};
}

constexpr std::array case_encodings{body(0), body(0x7f), body(0xff)};

inline void require(const bool good, const char *message) {
  if (!good) { throw std::runtime_error(message); }
}

struct Source {
  explicit Source(const Byte first_value) {
    bodies[0][1] = first_value;
    spans = {CodeSpan{.address = entry, .words = call},
             CodeSpan{.address = helper, .words = dispatch},
             CodeSpan{.address = cases[0], .words = bodies[0]},
             CodeSpan{.address = cases[1], .words = bodies[1]},
             CodeSpan{.address = cases[2], .words = bodies[2]}};
  }

  Source(const Source &) = delete;
  Source &operator=(const Source &) = delete;

  [[nodiscard]] CodeVariant variant(const std::uint64_t id) const {
    return {.id = id, .code = spans, .entries = entries};
  }

  const std::array<Byte, 3> call = call_encoding;
  const std::array<Byte, 5> dispatch = dispatch_encoding;
  std::array<std::array<Byte, 4>, 3> bodies = case_encodings;
  std::array<CodeSpan, 5> spans{};
  std::array<Address, 1> entries{entry};
};

struct Fixture {
  explicit Fixture(const Source &source) {
    bytes.fill(padding);
    for (const auto &span : source.spans) {
      std::ranges::copy(span.words, bytes.begin() + span.address);
    }
    constexpr std::array<Byte, 4> offsets{0x18, 0x18, 0x28, 0x30};
    std::ranges::copy(offsets, bytes.begin() + table);
    grants = {Ram{.bytes = std::span(bytes).first(entry)},
              Ram{.first = entry,
                  .bytes = std::span(bytes).subspan(entry, code_end - entry),
                  .writable = false},
              Ram{.first = code_end, .bytes = std::span(bytes).subspan(code_end)}};
  }

  [[nodiscard]] Memory memory() const { return Memory::mapped(grants); }

  std::array<Byte, address_bytes> bytes{};
  std::array<Ram, 3> grants{};
};

struct Owner {
  Owner(const ImageProgram &program, const std::uint64_t variant, const Memory &memory)
      : image(program), id(variant) {
    image.validate_binding(id, memory);
    for (const auto &region : memory.regions()) {
      bindings.push_back({.bytes = region.bytes.data(), .known = region.known.data()});
    }
    for (const auto &operation : image.instructions(id)) {
      const auto page = std::size_t{operation.pc} >> detail::native_page_shift;
      if (pages[page] == nullptr) {
        pages[page] = std::make_unique<detail::NativePage>();
        index[page] = pages[page]->data();
      }
      const auto offset = operation.pc % detail::native_page_entries;
      (*pages[page])[offset] = image.entry(id, operation.pc);
    }
    seed({});
  }

  void seed(const Registers registers) {
    context = {};
    context.cpu.registers = registers;
    context.memory_bindings = bindings.data();
  }

  [[nodiscard]] detail::NativeResult run(const std::uint32_t budget) {
    return detail::enter_blocks(
        image.entry(id, context.cpu.registers.pc), context.cpu.registers, context, budget, index);
  }

  const ImageProgram &image;
  std::uint64_t id;
  std::array<std::unique_ptr<detail::NativePage>, detail::native_page_entries> pages{};
  detail::NativeIndex index{};
  std::vector<detail::MemoryBinding> bindings;
  detail::NativeContext context{};
};

void rejected_images();
void binding_contract();
void alternate_streams();
void artifact_contract();
void wrapping_stream();
}
