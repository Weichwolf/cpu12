// SPDX-License-Identifier: GPL-3.0-only
#include "image.hpp"

#include "cpu12/core.hpp"
#include "jit/llvm/image.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpu12 {
namespace {
std::vector<InterruptRequest> interrupts(const std::span<const InterruptRequest> requests) {
  std::vector<InterruptRequest> result;
  result.reserve(requests.size());
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
}

NativeImage::Impl::Impl(const CodeImage image, const CompileOptions options)
    : interrupts(cpu12::interrupts(options.interrupts)),
      program(std::make_shared<const detail::llvm19::ImageProgram>(
          image, options.memory, std::span<const std::byte>{}, interrupts)),
      report(program->report()) {}

NativeImage::NativeImage(std::shared_ptr<const Impl> image) noexcept : impl_(std::move(image)) {}

PreparationReport NativeImage::report() const noexcept {
  return impl_ ? impl_->report : PreparationReport{};
}
}
