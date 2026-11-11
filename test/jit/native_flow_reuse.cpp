// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "jit/block.hpp"
#include "jit/image.hpp"
#include "jit/llvm/program.hpp"
#include "native_flow.hpp"

#include <array>
#include <span>
#include <vector>

namespace cpu12::test::native_flow {
namespace {
void indirect_binding() {
  constexpr Byte indirect_d_x = 0xe7;
  std::array<Byte, 2> code{static_cast<Byte>(isa::Opcode::jmp_indexed), indirect_d_x};
  const std::array spans{CodeSpan{.address = entry, .words = code}};
  const std::array<Address, 1> entries{entry};
  const std::array variants{CodeVariant{.id = variant_id, .code = spans, .entries = entries}};
  const detail::ImagePlan plan({.variants = variants});
  const auto &instructions = plan.variants()[0].instructions;
  std::array<Byte, 3> data{};
  constexpr Address data_first = 0x2000;
  const std::array original{Ram{.first = data_first, .bytes = data},
                            Ram{.first = entry, .bytes = code, .writable = false}};
  const std::array changed{Ram{.first = data_first, .bytes = std::span(data).first(2)},
                           original[1]};
  const detail::llvm19::Program program(instructions, Memory::mapped(original));
  require(program.find(instructions, Memory::mapped(original)) != nullptr &&
              program.find(instructions, Memory::mapped(changed)) == nullptr,
          "A standalone indirect JMP retains its actual pointer-read topology across rebinding");
}

void address_binding() {
  constexpr Byte stack_base = 0x80;
  std::array<Byte, 4> code{
      static_cast<Byte>(isa::Opcode::leax_indexed), stack_base, idle_code[0], idle_code[1]};
  const std::array spans{CodeSpan{.address = entry, .words = code}};
  const std::array<Address, 1> entries{entry};
  const std::array variants{CodeVariant{.id = variant_id, .code = spans, .entries = entries}};
  const detail::ImagePlan plan({.variants = variants});
  const auto &instructions = plan.variants()[0].instructions;
  std::array<Byte, 3> data{};
  constexpr Address data_first = 0x2000;
  const std::array original{Ram{.first = data_first, .bytes = data},
                            Ram{.first = entry, .bytes = code, .writable = false}};
  const std::array changed{Ram{.first = data_first, .bytes = std::span(data).first(2)},
                           original[1]};
  const detail::llvm19::Program program(instructions, Memory::mapped(original));
  const auto *compiled = program.find(instructions, Memory::mapped(original));
  require(
      compiled != nullptr && program.find(instructions, Memory::mapped(changed)) == compiled,
      "Pure LEA and local control flow retain native code across unrelated data topology changes");
}
}

void reuse() {
  indirect_binding();
  address_binding();
  const std::array variants{CodeVariant{.id = variant_id, .code = spans, .entries = entries}};
  const detail::ImagePlan plan({.variants = variants});
  const auto &instructions = plan.variants()[0].instructions;
  Fixture original;
  const detail::llvm19::Program program(instructions, original.memory());
  std::vector<detail::BlockInstruction> caller;
  std::vector<detail::BlockInstruction> independent;
  for (const auto &operation : instructions) {
    if (operation.pc < first) { caller.push_back(operation); }
    if (operation.pc == idle) { independent.push_back(operation); }
  }
  const auto *compiled = program.find(caller, original.memory());
  const auto *loop = program.find(independent, original.memory());
  require(compiled != nullptr && loop != nullptr,
          "Caller and independent loop remain genuine separate native units");
  Fixture changed;
  changed.bytes[first + 2] ^= 1;
  require(program.find(caller, changed.memory()) == nullptr &&
              program.find(independent, changed.memory()) == loop,
          "A changed native callee invalidates its caller while unrelated immutable code remains "
          "reusable");
  changed.bytes[first + 2] ^= 1;
  require(program.find(caller, changed.memory()) == compiled,
          "Exact original code and transitive bindings retain their native code ownership across "
          "rebinding");
  Fixture narrowed;
  narrowed.grants[1].bytes = std::span(narrowed.bytes).subspan(entry, first - entry);
  require(program.find(caller, narrowed.memory()) == nullptr,
          "A lost callee grant cannot route a retained caller into code outside its current source "
          "binding");
}
}
