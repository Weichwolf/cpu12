// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "jit/llvm/callback_binding.hpp"
#include "jit/llvm/image.hpp"

#include <cstddef>
#include <span>

namespace cpu12::test::image {
void callback_events(const detail::llvm19::ImageProgram &image);
void callback_masked(std::span<const std::byte> bindings,
                     std::span<const detail::llvm19::CallbackBinding> callbacks);
}
