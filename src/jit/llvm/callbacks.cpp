// SPDX-License-Identifier: GPL-3.0-only
#include "jit/llvm/callbacks.hpp"

#include "cpu12/core.hpp"
#include "jit/llvm/callback_binding.hpp"
#include "runtime.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/Types.h"
#include "llvm/ADT/APInt.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpu12::detail::llvm19 {
namespace {
constexpr std::array adapters{"cpu12_read_byte",
                              "cpu12_write_byte",
                              "cpu12_read_word",
                              "cpu12_write_word",
                              "cpu12_read_masked_byte"};

std::size_t offset(const CallbackRole role) {
  const auto field = [role] {
    switch (role) {
      case CallbackRole::read: return offsetof(Io, read);
      case CallbackRole::write: return offsetof(Io, write);
      case CallbackRole::read_word: return offsetof(Io, read_word);
      case CallbackRole::write_word: return offsetof(Io, write_word);
      case CallbackRole::read_masked: return offsetof(Io, read_masked);
      default: throw std::invalid_argument("Unknown CPU12 callback role");
    }
  };
  return offsetof(NativeContext, io) + field();
}

std::optional<std::size_t> slot(const llvm::LoadInst &load) {
  const auto *function = load.getFunction();
  if (function->arg_empty() || !load.getType()->isPointerTy() || load.isVolatile() ||
      load.isAtomic()) {
    return {};
  }
  const auto &layout = function->getParent()->getDataLayout();
  llvm::APInt at(layout.getIndexSizeInBits(load.getPointerAddressSpace()), 0);
  const auto *base = load.getPointerOperand()->stripAndAccumulateConstantOffsets(layout, at, false);
  if (base != function->getArg(0) || at.isNegative() ||
      at.getActiveBits() > sizeof(std::size_t) * 8) {
    return {};
  }
  return static_cast<std::size_t>(at.getZExtValue());
}

std::uint64_t attribute(const llvm::AttributeSet attributes, const llvm::Attribute::AttrKind kind) {
  const auto value = attributes.getAttribute(kind);
  return value.isValid() ? value.getValueAsInt() : 0;
}

void preconditions(const llvm::CallBase &call, const llvm::Function &function) {
  for (unsigned index = 0; index < call.arg_size(); ++index) {
    const auto expected = call.getAttributes().getParamAttrs(index);
    const auto required = function.getAttributes().getParamAttrs(index);
    for (const auto kind :
         {llvm::Attribute::NoAlias, llvm::Attribute::NonNull, llvm::Attribute::NoUndef}) {
      if (required.hasAttribute(kind) && !expected.hasAttribute(kind)) {
        throw std::invalid_argument("CPU12 callback has a stronger parameter contract: " +
                                    function.getName().str());
      }
    }
    for (const auto kind : {llvm::Attribute::Alignment,
                            llvm::Attribute::Dereferenceable,
                            llvm::Attribute::DereferenceableOrNull}) {
      if (attribute(required, kind) > attribute(expected, kind)) {
        throw std::invalid_argument("CPU12 callback requires stronger pointer guarantees: " +
                                    function.getName().str());
      }
    }
  }
}

void compatible(const llvm::CallBase &call, const llvm::Function &function) {
  if (call.getFunctionType() != function.getFunctionType() ||
      call.getCallingConv() != function.getCallingConv() || !function.doesNotThrow()) {
    throw std::invalid_argument("CPU12 callback ABI mismatch for " + function.getName().str());
  }
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
  for (unsigned index = 0; index <= call.arg_size(); ++index) {
    for (const auto attribute : attributes) {
      if (call.getAttributes().getAttributeAtIndex(index, attribute) !=
          function.getAttributes().getAttributeAtIndex(index, attribute)) {
        throw std::invalid_argument(
            "CPU12 callback ABI attribute mismatch for " + function.getName().str() +
            " index=" + std::to_string(index) +
            " attribute=" + llvm::Attribute::getNameFromAttrKind(attribute).str());
      }
    }
  }
  preconditions(call, function);
}

void validate(const llvm::LoadInst &load, const llvm::Function &target) {
  bool called = false;
  for (const auto *user : load.users()) {
    if (const auto *call = llvm::dyn_cast<llvm::CallBase>(user);
        call != nullptr && call->getCalledOperand() == &load) {
      compatible(*call, target);
      called = true;
    } else if (const auto *compare = llvm::dyn_cast<llvm::ICmpInst>(user);
               compare == nullptr || !compare->isEquality() ||
               (!llvm::isa<llvm::ConstantPointerNull>(compare->getOperand(0)) &&
                !llvm::isa<llvm::ConstantPointerNull>(compare->getOperand(1)))) {
      throw std::invalid_argument("CPU12 adapter callback has an unexpected pointer use");
    }
  }
  if (!called) { throw std::invalid_argument("CPU12 adapter callback has no typed call"); }
}

using Replacement = std::pair<llvm::LoadInst *, llvm::Function *>;

struct Target {
  llvm::Function *function;
  std::string_view symbol;
};

llvm::Function *declare(llvm::LoadInst &load, const std::string_view symbol) {
  for (const auto *user : load.users()) {
    if (const auto *call = llvm::dyn_cast<llvm::CallBase>(user);
        call != nullptr && call->getCalledOperand() == &load) {
      auto *target = llvm::Function::Create(
          call->getFunctionType(), llvm::GlobalValue::ExternalLinkage, symbol, load.getModule());
      LLVMSetFunctionCallConv(llvm::wrap(target), call->getCallingConv());
      target->setAttributes(call->getAttributes());
      return target;
    }
  }
  throw std::invalid_argument("CPU12 adapter callback has no typed call");
}

void collect(llvm::Function &adapter,
             Target &target,
             const std::size_t at,
             std::vector<Replacement> &replacements) {
  for (auto &block : adapter) {
    for (auto &instruction : block) {
      auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
      if (load == nullptr || slot(*load) != at) { continue; }
      if (target.function == nullptr) { target.function = declare(*load, target.symbol); }
      validate(*load, *target.function);
      replacements.emplace_back(load, target.function);
    }
  }
}
}

void link_callbacks(LLVMModuleRef module,
                    const std::span<const CallbackBinding> bindings,
                    const CallbackStage stage) {
  auto &ir = *llvm::unwrap(module);
  std::set<CallbackRole> roles;
  std::vector<Replacement> replacements;
  for (const auto binding : bindings) {
    const auto at = offset(binding.role);
    if (!roles.insert(binding.role).second || binding.symbol.empty() ||
        binding.symbol.find('\0') != std::string_view::npos) {
      throw std::invalid_argument("Invalid or duplicate CPU12 callback binding");
    }
    Target target{.function = ir.getFunction(binding.symbol), .symbol = binding.symbol};
    if ((stage == CallbackStage::definitions &&
         (target.function == nullptr || target.function->isDeclaration())) ||
        (target.function != nullptr && target.function->isVarArg())) {
      throw std::invalid_argument("CPU12 callback definition missing for " +
                                  std::string(binding.symbol));
    }
    const auto before = replacements.size();
    for (const auto *name : adapters) {
      auto *adapter = ir.getFunction(name);
      if (adapter == nullptr || adapter->isDeclaration() || adapter == target.function) {
        throw std::invalid_argument("CPU12 callback requires its core-owned adapter definition");
      }
      collect(*adapter, target, at, replacements);
    }
    if (replacements.size() == before) {
      throw std::invalid_argument("CPU12 callback role is absent from its core adapters");
    }
  }
  for (const auto [load, target] : replacements) {
    load->replaceAllUsesWith(target);
    load->eraseFromParent();
  }
}

void validate_callbacks(LLVMModuleRef module,
                        LLVMModuleRef incoming,
                        const std::span<const CallbackBinding> bindings) {
  auto &expected = *llvm::unwrap(module);
  auto &definitions = *llvm::unwrap(incoming);
  for (const auto binding : bindings) {
    const auto *declaration = expected.getFunction(binding.symbol);
    const auto *target = definitions.getFunction(binding.symbol);
    if (declaration == nullptr || target == nullptr || target->isDeclaration()) { continue; }
    for (const auto *user : declaration->users()) {
      if (const auto *call = llvm::dyn_cast<llvm::CallBase>(user);
          call != nullptr && call->getCalledOperand() == declaration) {
        compatible(*call, *target);
      }
    }
  }
}

void require_callbacks(LLVMModuleRef module, const std::span<const CallbackBinding> bindings) {
  const auto &ir = *llvm::unwrap(module);
  for (const auto binding : bindings) {
    const auto *target = ir.getFunction(binding.symbol);
    if (target == nullptr || target->isDeclaration() || target->isVarArg()) {
      throw std::invalid_argument("CPU12 callback definition missing for " +
                                  std::string(binding.symbol));
    }
  }
}
}
