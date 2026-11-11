// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "jit/block.hpp"
#include "jit/llvm/builder.hpp"

#include "llvm-c/Types.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace cpu12::detail::llvm19 {
class Flow {
public:
  Flow(LLVMModuleRef module,
       std::span<const std::vector<BlockInstruction>> regions,
       const Memory &memory,
       const std::string &prefix,
       bool native);
  [[nodiscard]] LLVMValueRef target(Address pc) const;

  [[nodiscard]] LLVMValueRef dispatcher() const { return dispatch_; }

  void
  transfer(const Builder &builder, LLVMValueRef target, std::span<LLVMValueRef> arguments) const;

private:
  [[nodiscard]] LLVMValueRef table(const std::string &prefix) const;
  void emit_dispatch(LLVMValueRef table) const;
  LLVMModuleRef module_;
  LLVMValueRef dispatch_;
  std::map<Address, LLVMValueRef> targets_;
};
}
