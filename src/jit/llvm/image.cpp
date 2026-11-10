// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/image.hpp"

#include "cpu12/memory.hpp"
#include "immutable.hpp"
#include "jit/block.hpp"
#include "jit/image.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/program.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace cpu12::detail::llvm19 {
namespace {
struct CompilerMemory {
  explicit CompilerMemory(const ImagePlan::Variant &variant,
                          const std::span<const RegionLayout> layout) {
    for (const auto &span : variant.code) {
      for (std::size_t offset = 0; offset < span.words.size(); ++offset) {
        const auto address = static_cast<Address>(span.address + offset);
        source[address] = span.words[offset];
        known[address] = 1;
      }
    }
    for (std::size_t first = 0; first < address_bytes;) {
      if (known[first] == 0) {
        ++first;
        continue;
      }
      auto end = first + 1;
      while (end < address_bytes && known[end] != 0) { ++end; }
      code.push_back({.first = static_cast<Address>(first),
                      .bytes = std::span(source).subspan(first, end - first),
                      .writable = false});
      first = end;
    }
    for (const auto &region : layout) {
      if (region.size == 0 || region.size > address_bytes - region.first) {
        throw std::invalid_argument("CPU12 image region is outside the address space");
      }
      memory.push_back({.first = region.first,
                        .bytes = std::span(scratch).subspan(region.first, region.size),
                        .known = region.tracked
                                     ? std::span(tracked).subspan(region.first, region.size)
                                     : std::span<Byte>{},
                        .writable = region.writable});
    }
    const auto bindings = Memory::mapped(memory);
    for (const auto &span : variant.code) {
      for (std::size_t offset = 0; offset < span.words.size(); ++offset) {
        const auto address = static_cast<Address>(span.address + offset);
        if (immutable_bytes(bindings, address).empty()) {
          throw std::invalid_argument("CPU12 variant=" + std::to_string(variant.id) +
                                      " PC=" + std::to_string(address) +
                                      ": executable source requires an immutable binding view");
        }
      }
    }
  }

  std::array<Byte, address_bytes> source{}, known{}, scratch{}, tracked{};
  std::vector<Ram> code, memory;
};

bool same_layout(const std::span<const RegionLayout> layout, const Memory &memory) {
  const auto regions = memory.regions();
  return layout.size() == regions.size() &&
         std::ranges::equal(layout, regions, [](const auto &expected, const auto &actual) {
           return expected.first == actual.first && expected.size == actual.bytes.size() &&
                  expected.writable == actual.writable && expected.tracked == !actual.known.empty();
         });
}
}

class ImageProgram::Impl {
public:
  explicit Impl(const CodeImage image,
                const std::span<const VariantLayout> layouts,
                const std::span<const std::byte> linked)
      : plan(image) {
    for (const auto layout : layouts) {
      if (!bindings.emplace(layout.id, std::vector(layout.memory.begin(), layout.memory.end()))
               .second) {
        throw std::invalid_argument("CPU12 image has duplicate variant memory layouts");
      }
    }
    if (bindings.size() != plan.variants().size()) {
      throw std::invalid_argument("CPU12 image requires one memory layout per variant");
    }
    for (const auto &variant : plan.variants()) {
      const auto found = bindings.find(variant.id);
      if (found == bindings.end()) {
        throw std::invalid_argument("CPU12 image variant has no memory layout");
      }
      for (const auto &operation : variant.instructions) {
        if (!supports(operation.instruction)) {
          throw std::invalid_argument("CPU12 variant=" + std::to_string(variant.id) +
                                      " PC=" + std::to_string(operation.pc) +
                                      ": instruction has no LLVM lowering");
        }
      }
    }
    for (const auto &variant : plan.variants()) {
      const auto &layout = bindings.at(variant.id);
      const auto memory = std::make_unique<CompilerMemory>(variant, layout);
      programs.emplace(
          variant.id,
          std::make_unique<Program>(
              variant.instructions,
              ProgramOptions{.memory = {.source = Memory::mapped(memory->code),
                                        .bindings = Memory::mapped(memory->memory)},
                             .prefix = "cpu12_variant_" + std::to_string(variant.id) + "_region_",
                             .bindings = linked,
                             .output = ArtifactOutput::native}));
    }
  }

  [[nodiscard]] const ImagePlan::Variant &variant(const std::uint64_t id) const {
    const auto variants = plan.variants();
    const auto found = std::ranges::find(variants, id, &ImagePlan::Variant::id);
    if (found == variants.end()) {
      throw std::invalid_argument("Unknown CPU12 image variant=" + std::to_string(id));
    }
    return *found;
  }

  const ImagePlan plan;
  std::map<std::uint64_t, std::vector<RegionLayout>> bindings;
  std::map<std::uint64_t, std::unique_ptr<Program>> programs;
};

ImageProgram::ImageProgram(const CodeImage image,
                           const std::span<const VariantLayout> layouts,
                           const std::span<const std::byte> bindings)
    : impl_(std::make_unique<Impl>(image, layouts, bindings)) {}

ImageProgram::~ImageProgram() = default;

const Program &ImageProgram::program(const std::uint64_t id) const {
  static_cast<void>(impl_->variant(id));
  return *impl_->programs.at(id);
}

const void *ImageProgram::entry(const std::uint64_t id, const Address pc) const {
  if (const auto *entry = program(id).find(pc)) { return entry; }
  throw std::invalid_argument("CPU12 variant=" + std::to_string(id) + " PC=" + std::to_string(pc) +
                              ": undeclared instruction entry");
}

std::span<const BlockInstruction> ImageProgram::instructions(const std::uint64_t id) const {
  return impl_->variant(id).instructions;
}

void ImageProgram::validate_binding(const std::uint64_t id, const Memory &memory) const {
  const auto &variant = impl_->variant(id);
  if (!same_layout(impl_->bindings.at(id), memory)) {
    throw std::invalid_argument("CPU12 image memory layout does not match variant=" +
                                std::to_string(id));
  }
  for (const auto &span : variant.code) {
    for (std::size_t offset = 0; offset < span.words.size(); ++offset) {
      const auto address = static_cast<Address>(span.address + offset);
      const auto bytes = immutable_bytes(memory, address);
      if (bytes.empty() || bytes.front() != span.words[offset]) {
        throw std::invalid_argument("CPU12 variant=" + std::to_string(id) + " PC=" +
                                    std::to_string(address) + ": executable source does not match");
      }
    }
  }
}
}
