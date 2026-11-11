// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/block.hpp"
#include "jit/llvm/image.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace cpu12::test::image {
using detail::llvm19::ImageProgram;

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
void semantic_contract();
void wrapping_stream();
}
