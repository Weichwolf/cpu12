// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string_view>

namespace cpu12::detail::llvm19 {
enum class CallbackRole : std::uint8_t { read, write, read_word, write_word, read_masked };

struct CallbackBinding {
  CallbackRole role;
  std::string_view symbol;
};
}
