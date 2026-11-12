// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/image.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "immutable.hpp"
#include "jit/block.hpp"
#include "jit/image.hpp"
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/callback_binding.hpp"
#include "jit/llvm/identity.hpp"
#include "jit/llvm/lift.hpp"
#include "jit/llvm/program.hpp"
#include "jit/llvm/runtime_bitcode.hpp"
#include "jit/prefixes.hpp"
#include "source_identity.hpp"

#include "llvm/Config/llvm-config.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

namespace {
struct ImageSource {
  ImageSource(const CodeImage image,
              const std::span<const VariantLayout> layouts,
              const std::span<const CodeTransition> transitions,
              const std::span<const InterruptRequest> interrupts)
      : plan(image, transitions), events(interrupts.begin(), interrupts.end()) {
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
      const auto memory = std::make_unique<CompilerMemory>(variant, found->second);
      prefixes.emplace(variant.id,
                       foreground_prefixes(variant.instructions, Memory::mapped(memory->memory)));
    }
  }

  const ImagePlan plan;
  const std::vector<InterruptRequest> events;
  std::map<std::uint64_t, std::vector<RegionLayout>> bindings;
  std::map<std::uint64_t, std::map<Address, std::uint64_t>> prefixes;

  [[nodiscard]] std::string
  namespace_prefix(const std::span<const InterruptRequest> interrupts,
                   const std::span<const CallbackBinding> callbacks) const {
    Identity hash;
    hash.add(source_identity);
    hash.add(LLVM_VERSION_STRING);
    hash.add(runtime_bitcode());
    hash.add(native_features());
    hash.add(plan.variants().size());
    for (const auto &variant : plan.variants()) {
      hash.add(variant.id);
      hash.add(variant.code.size());
      for (const auto &span : variant.code) {
        hash.add(span.address);
        hash.add(std::as_bytes(std::span(span.words)));
      }
      hash.add(variant.entries.size());
      for (const auto entry : variant.entries) { hash.add(entry); }
      const auto &layout = bindings.at(variant.id);
      hash.add(layout.size());
      for (const auto region : layout) {
        hash.add(region.first);
        hash.add(region.size);
        hash.add(static_cast<std::uint64_t>(region.writable));
        hash.add(static_cast<std::uint64_t>(region.tracked));
      }
      hash.add(variant.transitions.size());
      for (const auto &[entry, targets] : variant.transitions) {
        hash.add(entry);
        hash.add(targets.size());
        for (const auto target : targets) { hash.add(target); }
      }
    }
    hash.add(interrupts.size());
    for (const auto request : interrupts) {
      hash.add(request.vector);
      hash.add(static_cast<std::uint64_t>(request.kind));
    }
    hash.add(callbacks.size());
    for (const auto binding : callbacks) {
      hash.add(static_cast<std::uint64_t>(binding.role));
      hash.add(binding.symbol);
    }
    return "cpu12_" + hash.finish() + "_";
  }
};
}

class ImageProgram::Impl {
public:
  Impl(const CodeImage image,
       const std::span<const VariantLayout> layouts,
       const std::span<const std::byte> linked,
       const std::span<const InterruptRequest> interrupts,
       const std::span<const CodeTransition> transitions,
       const std::span<const CallbackBinding> callbacks,
       const ArtifactOutput output,
       const std::string &prefix)
      : source(std::make_shared<const ImageSource>(image, layouts, transitions, interrupts)) {
    const auto names = prefix.empty() ? source->namespace_prefix(interrupts, callbacks) : prefix;
    for (const auto &variant : source->plan.variants()) {
      const auto memory =
          std::make_unique<CompilerMemory>(variant, source->bindings.at(variant.id));
      programs.emplace(
          variant.id,
          std::make_unique<Program>(
              variant.instructions,
              ProgramOptions{.memory = {.source = Memory::mapped(memory->code),
                                        .bindings = Memory::mapped(memory->memory)},
                             .prefix = names + "variant_" + std::to_string(variant.id) + "_region_",
                             .bindings = linked,
                             .output = output,
                             .interrupts = interrupts,
                             .prepare_events = true,
                             .runtime_io = output == ArtifactOutput::semantic,
                             .registers = output == ArtifactOutput::semantic
                                              ? RegisterSource::owner
                                              : RegisterSource::argument,
                             .callbacks = callbacks}));
    }
  }

  Impl(const Impl &semantic, const NativeSymbols &symbols)
      : source(semantic.source), native_bytes(symbols.native_bytes) {
    for (const auto &variant : source->plan.variants()) {
      programs.emplace(variant.id,
                       std::make_unique<Program>(
                           *semantic.programs.at(variant.id), variant.instructions, symbols));
    }
  }

  [[nodiscard]] const ImagePlan::Variant &variant(const std::uint64_t id) const {
    const auto variants = source->plan.variants();
    const auto found = std::ranges::find(variants, id, &ImagePlan::Variant::id);
    if (found == variants.end()) {
      throw std::invalid_argument("Unknown CPU12 image variant=" + std::to_string(id));
    }
    return *found;
  }

  std::shared_ptr<const ImageSource> source;
  std::optional<std::uint64_t> native_bytes;
  std::map<std::uint64_t, std::unique_ptr<Program>> programs;
};

ImageProgram::ImageProgram(const CodeImage image,
                           const std::span<const VariantLayout> layouts,
                           const std::span<const std::byte> bindings,
                           const std::span<const InterruptRequest> interrupts,
                           const std::span<const CodeTransition> transitions,
                           const std::span<const CallbackBinding> callbacks,
                           const ArtifactOutput output,
                           const std::string &prefix)
    : impl_(std::make_unique<Impl>(
          image, layouts, bindings, interrupts, transitions, callbacks, output, prefix)) {}

ImageProgram::ImageProgram(const ImageProgram &semantic, const NativeSymbols &symbols)
    : impl_(std::make_unique<Impl>(*semantic.impl_, symbols)) {}

ImageProgram::~ImageProgram() = default;

const CodeTargets &ImageProgram::transitions(const std::uint64_t id) const {
  return impl_->variant(id).transitions;
}

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

PreparationReport ImageProgram::report() const noexcept {
  PreparationReport result;
  for (const auto &variant : impl_->source->plan.variants()) {
    result.prepared_entries += variant.instructions.size();
    result.native_bytes += impl_->programs.at(variant.id)->native_bytes();
  }
  result.native_bytes = impl_->native_bytes.value_or(result.native_bytes);
  return result;
}

std::span<const ImagePlan::Variant> ImageProgram::variants() const noexcept {
  return impl_->source->plan.variants();
}

std::span<const RegionLayout> ImageProgram::layout(const std::uint64_t id) const {
  static_cast<void>(impl_->variant(id));
  return impl_->source->bindings.at(id);
}

std::uint64_t ImageProgram::foreground_prefix(const std::uint64_t id, const Address pc) const {
  return impl_->source->prefixes.at(id).at(pc);
}

std::uint64_t ImageProgram::interrupt_entry_prefix(const std::uint64_t id,
                                                   const State &state,
                                                   const Memory &memory) const {
  validate_binding(id, memory);
  return detail::interrupt_entry_prefix(
      state, memory, impl_->source->prefixes.at(id), impl_->source->events);
}

void ImageProgram::validate_binding(const std::uint64_t id, const Memory &memory) const {
  const auto &variant = impl_->variant(id);
  if (!same_layout(impl_->source->bindings.at(id), memory)) {
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
