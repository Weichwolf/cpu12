// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "entries.hpp"
#include "jit/block.hpp"
#include "runtime.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

namespace cpu12::test::static_image {
namespace {
using image::Fixture;
using image::require;
constexpr Registers initial{
    .pc = image::entry, .d = 0xa500, .x = 0x1234, .y = 0x5678, .sp = 0x9000, .ccr = 0x81};
constexpr std::array<unsigned, 3> budgets{5, 17, 128};

void execute(const std::span<const Binding> code, Fixture &fixture, const Byte value) {
  std::array<std::unique_ptr<detail::NativePage>, detail::native_page_entries> pages{};
  detail::NativeIndex index{};
  for (const auto binding : code) {
    const auto page = std::size_t{binding.pc} >> detail::native_page_shift;
    if (!pages[page]) {
      pages[page] = std::make_unique<detail::NativePage>();
      index[page] = pages[page]->data();
    }
    (*pages[page])[binding.pc % detail::native_page_entries] =
        std::bit_cast<const void *>(binding.code);
  }
  std::vector<detail::MemoryBinding> bindings;
  bindings.reserve(fixture.grants.size());
  for (const auto &region : fixture.grants) {
    bindings.push_back({.bytes = region.bytes.data(), .known = region.known.data()});
  }
  const auto source = fixture.bytes;
  for (const auto budget : budgets) {
    fixture.bytes = source;
    detail::NativeContext owner{};
    owner.cpu.registers = initial;
    owner.memory_bindings = bindings.data();
    const auto offset = fixture.bytes[image::table];
    auto expected = initial;
    expected.pc = static_cast<Address>(image::table + offset + 2);
    expected.d = static_cast<Address>((unsigned{value} << image::byte_bits) | offset);
    expected.x = image::table;
    expected.ccr =
        static_cast<Byte>(initial.ccr | (value >= 0x80 ? 8U : 0U) | (value == 0 ? 4U : 0U));
    auto memory = source;
    memory[initial.sp - 2] = static_cast<Byte>(image::table >> image::byte_bits);
    memory[initial.sp - 1] = static_cast<Byte>(image::table);
    const auto *entry =
        index[initial.pc >> detail::native_page_shift][initial.pc % detail::native_page_entries];
    const auto result = detail::enter_blocks(entry, owner.cpu.registers, owner, budget, index);
    require(
        result.retired == budget && owner.cpu == State{.registers = expected, .failure = {}} &&
            owner.progress == Progress{.estimated_cycles = 14 + (std::uint64_t{budget - 5} * 3)} &&
            fixture.bytes == memory,
        "Build-time native objects preserve exact original code/data reads, owner state and RAM");
  }
}
}
}

int main() {
  try {
    using namespace cpu12::test;
    image::Fixture first{image::Source(0x80)};
    image::Fixture second{image::Source(0)};
    image::Fixture independent{image::Source(0x80)};
    constexpr cpu12::Byte negative = 0xff;
    independent.bytes[image::table] = independent.bytes[image::table + 3];
    static_image::execute(static_image::variant_7, first, 0x80);
    static_image::execute(static_image::variant_9, second, 0);
    static_image::execute(static_image::variant_7, independent, negative);
    std::cout << "Verified statically linked image variants in independent native owners.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
