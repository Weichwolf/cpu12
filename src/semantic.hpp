// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "cpu12/core.hpp"
#include "jit/llvm/image.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace cpu12 {
class SemanticImage::Impl {
public:
  Impl(CodeImage image, CompileOptions options);
  Impl(CodeImage image, CompileOptions options, std::span<const std::byte> bytes);
  void serialize(CodeImage image, CompileOptions options);
  const std::vector<InterruptRequest> interrupts;
  std::vector<std::string> callback_names;
  std::vector<CallbackBinding> callbacks;
  std::shared_ptr<const detail::llvm19::ImageProgram> program;
  std::vector<std::byte> bitcode, artifact;
  std::map<std::string, std::string> names;
  std::vector<SemanticExport> exports;
  std::string identity;

private:
  explicit Impl(CompileOptions options);
};
}
