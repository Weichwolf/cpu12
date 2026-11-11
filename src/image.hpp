// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"

#include <memory>
#include <span>
#include <vector>

namespace cpu12::detail::llvm19 {
class ImageProgram;
}

namespace cpu12 {
class NativeImage::Impl {
public:
  Impl(std::span<const InterruptRequest> requests,
       std::shared_ptr<const detail::llvm19::ImageProgram> image);
  const std::vector<InterruptRequest> interrupts;
  const std::shared_ptr<const detail::llvm19::ImageProgram> program;
  const PreparationReport report;
};
}
