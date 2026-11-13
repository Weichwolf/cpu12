// SPDX-License-Identifier: GPL-3.0-only
#include "semantic.hpp"

#include "cpu12/core.hpp"
#include "jit/interrupt.hpp"
#include "jit/llvm/abi.hpp"
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/identity.hpp"
#include "jit/llvm/image.hpp"
#include "jit/llvm/lifetime.hpp"
#include "jit/llvm/program.hpp"
#include "jit/llvm/runtime_bitcode.hpp"

#include "llvm-c/BitReader.h"
#include "llvm-c/Core.h"
#include "llvm-c/Linker.h"
#include "llvm-c/Types.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpu12 {
namespace {
template <class Ref, auto Dispose> using Owner = detail::llvm19::Owner<Ref, Dispose>;

std::vector<InterruptRequest> checked_interrupts(const std::span<const InterruptRequest> requests) {
  std::vector<InterruptRequest> result;
  for (const auto request : requests) {
    if (request.kind != InterruptKind::maskable && request.kind != InterruptKind::xirq) {
      throw std::invalid_argument("CPU12 image has an invalid interrupt kind");
    }
    if (std::ranges::any_of(result, [&](const auto before) {
          return before.vector == request.vector && before.kind == request.kind;
        })) {
      throw std::invalid_argument("CPU12 image has duplicate interrupt contracts");
    }
    result.push_back(request);
  }
  return result;
}

void merge_variant(LLVMModuleRef destination, const std::span<const std::byte> bytes) {
  const Owner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> buffer(
      LLVMCreateMemoryBufferWithMemoryRangeCopy(
          reinterpret_cast<const char *>(bytes.data()), bytes.size(), "cpu12-variant"));
  LLVMModuleRef raw = nullptr;
  const auto failed =
      LLVMParseBitcodeInContext2(LLVMGetModuleContext(destination), buffer.get(), &raw);
  Owner<LLVMModuleRef, LLVMDisposeModule> module(raw);
  if (failed != 0 || !module || LLVMLinkModules2(destination, module.release()) != 0) {
    throw std::runtime_error("Cannot compose the lowered CPU12 variants");
  }
}

void add_export(std::map<std::string, std::string> &exports,
                LLVMModuleRef module,
                const std::string_view name) {
  const std::string symbol(name);
  auto *function = LLVMGetNamedFunction(module, symbol.c_str());
  if (function == nullptr || LLVMIsDeclaration(function) != 0) {
    throw std::runtime_error("CPU12 semantic export is missing: " + symbol);
  }
  exports.emplace(symbol, detail::llvm19::function_abi(function));
}
}

SemanticImage::Impl::Impl(const CodeImage image, const CompileOptions options)
    : interrupts(checked_interrupts(options.interrupts)) {
  callback_names.reserve(options.callbacks.size());
  for (const auto binding : options.callbacks) { callback_names.emplace_back(binding.symbol); }
  for (std::size_t i = 0; i < options.callbacks.size(); ++i) {
    callbacks.push_back({.role = options.callbacks[i].role, .symbol = callback_names[i]});
  }
  program =
      std::make_shared<const detail::llvm19::ImageProgram>(image,
                                                           options.memory,
                                                           std::span<const std::byte>{},
                                                           interrupts,
                                                           options.transitions,
                                                           callbacks,
                                                           detail::llvm19::ArtifactOutput::semantic,
                                                           "",
                                                           options.topology);
  const detail::llvm19::Owner<LLVMContextRef, LLVMContextDispose> context(LLVMContextCreate());
  const detail::llvm19::Owner<LLVMModuleRef, LLVMDisposeModule> module(
      LLVMModuleCreateWithNameInContext("cpu12-image", context.get()));
  const auto target = detail::llvm19::runtime_target();
  LLVMSetTarget(module.get(), target.triple.c_str());
  LLVMSetDataLayout(module.get(), target.layout.c_str());
  for (const auto &variant : program->variants()) {
    merge_variant(module.get(), program->program(variant.id).bitcode());
  }
  constexpr std::array frames{detail::InterruptFrame::push,
                              detail::InterruptFrame::waiting,
                              detail::InterruptFrame::returned};
  for (const auto &variant : program->variants()) {
    const auto &lowered = program->program(variant.id);
    for (const auto &instruction : variant.instructions) {
      add_export(names, module.get(), lowered.symbol(instruction.pc));
    }
    for (const auto request : interrupts) {
      for (const auto frame : frames) {
        add_export(names, module.get(), lowered.event_symbol(detail::event_key(request, frame)));
      }
    }
    add_export(names, module.get(), lowered.event_symbol(detail::stop_event_key));
  }
  bitcode = detail::llvm19::bitcode(module.get());
  for (const auto &[name, abi] : names) { exports.push_back({.name = name, .abi = abi}); }
  detail::llvm19::Identity hash;
  hash.add(bitcode);
  for (const auto &[name, abi] : names) {
    hash.add(name);
    hash.add(abi);
  }
  identity = hash.finish();
}

SemanticImage::SemanticImage(std::shared_ptr<const Impl> image) noexcept
    : impl_(std::move(image)) {}

std::span<const std::byte> SemanticImage::bitcode() const noexcept {
  return impl_ ? std::span<const std::byte>(impl_->bitcode) : std::span<const std::byte>{};
}

std::span<const SemanticExport> SemanticImage::exports() const noexcept {
  return impl_ ? std::span<const SemanticExport>(impl_->exports)
               : std::span<const SemanticExport>{};
}

std::string_view SemanticImage::identity() const noexcept {
  return impl_ ? std::string_view(impl_->identity) : std::string_view{};
}
}
