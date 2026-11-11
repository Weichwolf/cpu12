// SPDX-License-Identifier: GPL-3.0-only
#include "image.hpp"

#include "cpu12/core.hpp"
#include "jit/llvm/image.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace cpu12 {
NativeImage::Impl::Impl(const std::span<const InterruptRequest> requests,
                        std::shared_ptr<const detail::llvm19::ImageProgram> image)
    : interrupts(requests.begin(), requests.end()),
      program(std::move(image)),
      report(program->report()) {}

NativeImage::NativeImage(std::shared_ptr<const Impl> image) noexcept : impl_(std::move(image)) {}

PreparationReport NativeImage::report() const noexcept {
  return impl_ ? impl_->report : PreparationReport{};
}
}
