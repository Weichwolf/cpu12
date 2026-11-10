// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_support.hpp"
#include "jit/block.hpp"
#include "jit/llvm/image.hpp"
#include "jit/preparation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace cpu12::test::image {
namespace {
constexpr std::uint64_t first_id = 7, second_id = 9;
constexpr Byte negative = 0x80;
constexpr unsigned high_mask = 0xff00;
constexpr std::array<unsigned, 11> grants{0, 1, 2, 3, 4, 5, 6, 7, 8, 17, 128};
constexpr std::array<Byte, 8> flags{0, 1, 4, 8, 0x10, 0x40, 0x80, 0xff};
constexpr Registers initial{.pc = entry, .d = 0xa500, .x = 0x1234, .y = 0x5678, .sp = 0x9000};

struct Expected {
  Registers registers;
  std::uint64_t cycles = 0;
};

Expected predict(const Registers before, const unsigned grant, const Fixture &fixture) {
  Expected result{.registers = before};
  auto &r = result.registers;
  const auto offset = fixture.bytes[table + (before.d & byte_mask)];
  const auto destination = static_cast<Address>(table + offset);
  if (grant >= 1) {
    r.pc = helper;
    r.sp -= 2;
    result.cycles += 4;
  }
  if (grant >= 2) {
    r.pc = helper + 1;
    r.sp += 2;
    r.x = table;
    result.cycles += 3;
  }
  if (grant >= 3) {
    constexpr unsigned retained_flags = 0xf1;
    r.pc = helper + 3;
    r.d = static_cast<Address>((r.d & high_mask) | offset);
    r.ccr = static_cast<Byte>(r.ccr & retained_flags);
    result.cycles += 3;
  }
  if (grant >= 4) {
    r.pc = destination;
    result.cycles += 3;
  }
  if (grant >= 5) {
    const auto value = fixture.bytes[destination + 1];
    r.pc = static_cast<Address>(destination + 2);
    r.d = static_cast<Address>((static_cast<unsigned>(value) << byte_bits) | offset);
    r.ccr = static_cast<Byte>(r.ccr | (value == 0 ? 4U : 0U) | (value >= negative ? 8U : 0U));
    result.cycles += 1 + ((grant - 5U) * 3U);
  }
  return result;
}

std::size_t run_prefixes(Owner &owner, Fixture &fixture) {
  const auto original = fixture.bytes;
  std::size_t count = 0;
  for (unsigned selection = 0; selection < 4; ++selection) {
    for (const auto ccr : flags) {
      for (const auto grant : grants) {
        fixture.bytes = original;
        auto before = initial;
        before.d = static_cast<Address>(before.d | selection);
        before.ccr = ccr;
        owner.seed(before);
        const auto expected = predict(before, grant, fixture);
        auto memory = original;
        if (grant != 0) {
          memory[before.sp - 2] = static_cast<Byte>(table >> byte_bits);
          memory[before.sp - 1] = static_cast<Byte>(table);
        }
        const auto result = owner.run(grant);
        require(result.retired == grant &&
                    owner.context.cpu == State{.registers = expected.registers, .failure = {}} &&
                    owner.context.progress.estimated_cycles == expected.cycles &&
                    owner.context.fault == Fault::none && fixture.bytes == memory,
                "Complete image uses actual JSR stack/table data and independently modeled native "
                "prefixes");
        ++count;
      }
    }
  }
  return count;
}

std::size_t two_owners() {
  std::unique_ptr<ImageProgram> image;
  std::unique_ptr<Fixture> first;
  std::unique_ptr<Fixture> second;
  {
    const Source original(negative);
    const Source changed(0);
    first = std::make_unique<Fixture>(original);
    second = std::make_unique<Fixture>(changed);
    const std::array variants{original.variant(first_id), changed.variant(second_id)};
    const std::array layouts{VariantLayout{.id = first_id, .memory = layout},
                             VariantLayout{.id = second_id, .memory = layout}};
    const auto legacy = detail::preparation_graph(first->memory(), original.entries);
    require(legacy.failure.fault == Fault::unsupported_instruction && legacy.failure.pc == table,
            "A raw call-fallthrough graph demonstrates why inline data must not become executable "
            "code");
    const auto before = first->bytes;
    image = std::make_unique<ImageProgram>(CodeImage{.variants = variants}, layouts);
    require(first->bytes == before && image->instructions(first_id).size() == 10 &&
                image->instructions(second_id).size() == 10 &&
                image->program(first_id).native_bytes() != 0 &&
                image->program(second_id).native_bytes() != 0 &&
                !image->program(first_id).bitcode().empty() &&
                !image->program(second_id).object().empty() &&
                image->program(first_id).symbol(entry) != image->program(second_id).symbol(entry),
            "Owner-free image compilation covers every declared variant/site without touching "
            "consumer RAM");
  }
  Owner first_owner(*image, first_id, first->memory());
  Owner second_owner(*image, second_id, second->memory());
  auto count = run_prefixes(first_owner, *first) + run_prefixes(second_owner, *second);
  Fixture independent{Source(negative)};
  independent.bytes[table] = independent.bytes[table + 2];
  Owner independent_owner(*image, first_id, independent.memory());
  count += run_prefixes(independent_owner, independent);
  bool rejected = false;
  try {
    static_cast<void>(image->entry(first_id, table));
  } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "Inline data has no native instruction entry even after source inputs expire");
  return count;
}
}
}

int main() {
  try {
    using namespace cpu12::test::image;
    const auto count = two_owners();
    rejected_images();
    binding_contract();
    alternate_streams();
    artifact_contract();
    wrapping_stream();
    std::cout << "Verified " << count
              << " complete-image native prefixes across variants/owners, "
                 "inline tables, expired inputs, exact RAM and rejected declarations/bindings.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
