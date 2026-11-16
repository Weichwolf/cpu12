// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "jit/llvm/identity.hpp"
#include "jit/llvm/image.hpp"
#include "semantic.hpp"
#include "serialization.hpp"
#include "source_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cpu12 {
namespace {
constexpr std::string_view format = "cpu12.semantic.1";

std::string fingerprint(const CodeImage image, const CompileOptions options) {
  detail::llvm19::Identity hash;
  hash.add(image.variants.size());
  for (const auto &variant : image.variants) {
    hash.add(variant.id);
    hash.add(variant.code.size());
    for (const auto span : variant.code) {
      hash.add(span.address);
      hash.add(std::as_bytes(span.words));
    }
    hash.add(variant.entries.size());
    for (const auto pc : variant.entries) { hash.add(pc); }
  }
  hash.add(options.memory.size());
  for (const auto &variant : options.memory) {
    hash.add(variant.id);
    hash.add(variant.memory.size());
    for (const auto region : variant.memory) {
      hash.add(region.first);
      hash.add(region.size);
      hash.add(static_cast<std::uint64_t>(region.writable));
      hash.add(static_cast<std::uint64_t>(region.tracked));
    }
  }
  hash.add(options.interrupts.size());
  for (const auto request : options.interrupts) {
    hash.add(request.vector);
    hash.add(static_cast<std::uint64_t>(request.kind));
  }
  hash.add(options.transitions.size());
  for (const auto edge : options.transitions) {
    hash.add(edge.from);
    hash.add(edge.to);
    hash.add(edge.entry);
  }
  hash.add(options.callbacks.size());
  for (const auto binding : options.callbacks) {
    hash.add(static_cast<std::uint64_t>(binding.role));
    hash.add(binding.symbol);
  }
  hash.add(options.bindings);
  hash.add(static_cast<std::uint64_t>(options.topology));
  return hash.finish();
}
}

std::string_view Core::compiler_identity() noexcept {
  return detail::llvm19::source_identity;
}

SemanticImage Core::load(const CodeImage image,
                         const CompileOptions options,
                         const std::span<const std::byte> artifact) {
  return SemanticImage(std::make_shared<const SemanticImage::Impl>(image, options, artifact));
}

void SemanticImage::Impl::serialize(const CodeImage image, const CompileOptions options) {
  detail::ArtifactWriter writer;
  writer.text(format);
  writer.text(Core::compiler_identity());
  writer.text(fingerprint(image, options));
  writer.text(identity);
  writer.bytes(bitcode);
  writer.sequence(names, [&](const auto &entry) {
    writer.text(entry.first);
    writer.text(entry.second);
  });
  program->serialize(writer);
  artifact = writer.finish();
}

SemanticImage::Impl::Impl(const CodeImage image,
                          const CompileOptions options,
                          const std::span<const std::byte> bytes)
    : Impl(options) {
  detail::ArtifactReader reader(bytes);
  if (reader.text() != format) {
    throw std::invalid_argument("CPU12 semantic artifact format mismatch");
  }
  if (reader.text() != Core::compiler_identity()) {
    throw std::invalid_argument("CPU12 semantic artifact compiler identity mismatch");
  }
  if (reader.text() != fingerprint(image, options)) {
    throw std::invalid_argument("CPU12 semantic artifact input identity mismatch");
  }
  identity = reader.text();
  const auto code = reader.bytes();
  if (identity.empty() || code.empty()) { detail::ArtifactReader::fail(); }
  bitcode.assign(code.begin(), code.end());
  reader.sequence([&] {
    auto name = reader.text();
    auto abi = reader.text();
    if (name.empty() || abi.empty() || !names.emplace(std::move(name), std::move(abi)).second) {
      detail::ArtifactReader::fail();
    }
  });
  if (names.empty()) { detail::ArtifactReader::fail(); }
  program = std::make_shared<const detail::llvm19::ImageProgram>(reader);
  reader.finish();
  for (const auto &[name, abi] : names) { exports.push_back({.name = name, .abi = abi}); }
  artifact.assign(bytes.begin(), bytes.end());
}

std::span<const std::byte> SemanticImage::artifact() const noexcept {
  return impl_ ? std::span<const std::byte>(impl_->artifact) : std::span<const std::byte>{};
}
}
