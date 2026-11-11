// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/abi.hpp"

#include "llvm-c/Types.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <string>

namespace cpu12::detail::llvm19 {
std::string function_abi(LLVMValueRef value) {
  const auto &function = *llvm::cast<llvm::Function>(llvm::unwrap(value));
  std::string result;
  llvm::raw_string_ostream text(result);
  text << "llvm19;cc=" << function.getCallingConv() << ";type=";
  function.getFunctionType()->print(text);
  constexpr std::array attributes{llvm::Attribute::SExt,
                                  llvm::Attribute::ZExt,
                                  llvm::Attribute::InReg,
                                  llvm::Attribute::StructRet,
                                  llvm::Attribute::ByVal,
                                  llvm::Attribute::ByRef,
                                  llvm::Attribute::InAlloca,
                                  llvm::Attribute::Preallocated,
                                  llvm::Attribute::Nest,
                                  llvm::Attribute::SwiftSelf,
                                  llvm::Attribute::SwiftError,
                                  llvm::Attribute::SwiftAsync,
                                  llvm::Attribute::Returned};
  for (unsigned index = 0; index <= function.arg_size(); ++index) {
    for (const auto kind : attributes) {
      const auto attribute = function.getAttributes().getAttributeAtIndex(index, kind);
      if (attribute.isValid()) { text << ';' << index << ':' << attribute.getAsString(); }
    }
  }
  return result;
}
}
