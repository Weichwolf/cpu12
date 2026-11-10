// SPDX-License-Identifier: GPL-3.0-only
#include "jit/image.hpp"

#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"
#include "jit/preparation.hpp"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cpu12::detail {
namespace {
[[noreturn]] void reject(const std::uint64_t id, const Address pc, const std::string_view reason) {
  throw std::invalid_argument("CPU12 code variant=" + std::to_string(id) +
                              " PC=" + std::to_string(pc) + ": " + std::string(reason));
}

struct Source {
  std::array<Byte, address_bytes> bytes{};
  std::bitset<address_bytes> known;
};

void collect(Source &source, const CodeVariant variant, const CodeSpan span) {
  if (span.words.empty() || span.words.size() > address_bytes) {
    reject(variant.id, span.address, "executable span must contain1..65536 bytes");
  }
  for (std::size_t offset = 0; offset < span.words.size(); ++offset) {
    const auto address = static_cast<Address>(span.address + offset);
    if (source.known[address] && source.bytes[address] != span.words[offset]) {
      reject(variant.id, address, "overlapping executable spans disagree");
    }
    source.bytes[address] = span.words[offset];
    source.known[address] = true;
  }
}

void decode(std::map<Address, BlockInstruction> &instructions,
            const CodeVariant variant,
            const CodeSpan span) {
  for (std::size_t offset = 0; offset < span.words.size();) {
    const auto pc = static_cast<Address>(span.address + offset);
    const auto instruction = decode_first(span.words.subspan(offset));
    if (!instruction) { reject(variant.id, pc, "invalid or truncated executable instruction"); }
    const auto found = instructions.find(pc);
    if (found != instructions.end() && found->second.instruction.length != instruction->length) {
      reject(variant.id, pc, "conflicting instruction boundaries");
    }
    instructions.insert_or_assign(pc, BlockInstruction{.instruction = *instruction, .pc = pc});
    offset += instruction->length;
  }
}

void validate(const std::map<Address, BlockInstruction> &instructions, const CodeVariant variant) {
  for (const auto entry : variant.entries) {
    if (!instructions.contains(entry)) {
      reject(variant.id, entry, "entry is not a declared instruction boundary");
    }
  }
  for (const auto &[pc, operation] : instructions) {
    const auto flow = successors(operation);
    const auto first = operation.instruction.kind == Kind::call ? 1U : 0U;
    for (unsigned edge = first; edge < flow.count; ++edge) {
      if (!instructions.contains(flow.direct[edge])) {
        reject(variant.id,
               pc,
               "direct successor=" + std::to_string(flow.direct[edge]) +
                   " is not a declared instruction boundary");
      }
    }
  }
}

ImagePlan::Variant analyze(const CodeVariant variant) {
  if (variant.code.empty() || variant.entries.empty()) {
    reject(variant.id, 0, "code spans and entries must be nonempty");
  }
  Source source;
  for (const auto span : variant.code) { collect(source, variant, span); }
  std::map<Address, BlockInstruction> instructions;
  for (const auto span : variant.code) { decode(instructions, variant, span); }
  validate(instructions, variant);
  ImagePlan::Variant result{.id = variant.id,
                            .code = {},
                            .instructions = {},
                            .entries = {variant.entries.begin(), variant.entries.end()}};
  result.instructions.reserve(instructions.size());
  for (const auto &[pc, operation] : instructions) { result.instructions.push_back(operation); }
  for (const auto span : variant.code) {
    result.code.push_back(
        {.address = span.address, .words = {span.words.begin(), span.words.end()}});
  }
  std::ranges::sort(result.entries);
  const auto unique = std::ranges::unique(result.entries);
  result.entries.erase(unique.begin(), unique.end());
  return result;
}
}

ImagePlan::ImagePlan(const CodeImage image) {
  if (image.variants.empty()) { throw std::invalid_argument("CPU12 code image has no variants"); }
  for (const auto variant : image.variants) {
    if (std::ranges::any_of(variants_, [&](const auto &other) { return other.id == variant.id; })) {
      reject(variant.id, 0, "duplicate variant identity");
    }
    variants_.push_back(analyze(variant));
  }
}
}
