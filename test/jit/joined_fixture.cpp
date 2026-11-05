// SPDX-License-Identifier: GPL-3.0-only
#include "joined_fixture.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/code.hpp"
#include "jit/emission.hpp"
#include "jit/emitter.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cpu12::test::joined {
namespace {
extern "C" std::uint32_t
cpu12_joined_test_enter(const void *, Registers *, detail::NativeContext *) noexcept;
asm(".pushsection .text\n"
    ".globl cpu12_joined_test_enter\n"
    ".hidden cpu12_joined_test_enter\n"
    ".type cpu12_joined_test_enter,@function\n"
    "cpu12_joined_test_enter:\n"
    "endbr64\n"
    "mov %rdi,%rax\n"
    "mov %rsi,%rdi\n"
    "mov %rdx,%rsi\n"
    "jmp *%rax\n"
    ".size cpu12_joined_test_enter,.-cpu12_joined_test_enter\n"
    ".popsection\n");

struct Fixup {
  std::size_t displacement = 0, instruction = 0;
  detail::NativeExit kind = detail::NativeExit::complete;
};

void return_edge(detail::Code &code, const Edge edge) {
  // Native MOV EAX, tag; RET. No guest instruction or retirement is fabricated.
  constexpr Byte mov_eax = 0xb8;
  constexpr Byte ret = 0xc3;
  code.byte(mov_eax);
  code.u32(static_cast<std::uint32_t>(edge));
  code.byte(ret);
}
} // namespace

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

detail::NativeContext fresh_context() {
  detail::NativeContext result{};
  result.cpu.registers = seed;
  return result;
}

Translation::Translation(const std::initializer_list<Encoded> instructions, const Memory &memory) {
  require(instructions.size() != 0, "Test translation needs an authored instruction");
  detail::Code code;
  constexpr std::array<Byte, 4> endbr64{0xf3, 0x0f, 0x1e, 0xfa};
  code.raw(endbr64);
  std::vector<std::size_t> starts;
  std::vector<Fixup> fixups;
  for (const auto &encoded : instructions) {
    const auto instruction = detail::decode(encoded);
    if (!instruction) { throw std::runtime_error("Authored joined instruction does not decode"); }
    const auto fragment = detail::emit_joined(*instruction, memory);
    const auto offset = code.size();
    starts.push_back(offset);
    code.raw(fragment.bytes);
    require(!fragment.exits.empty(), "Every emitted instruction describes its actual exits");
    for (const auto exit : fragment.exits) {
      fixups.push_back({.displacement = offset + exit.displacement,
                        .instruction = starts.size() - 1,
                        .kind = exit.kind});
    }
  }
  const auto complete = code.size();
  return_edge(code, Edge::complete);
  const auto fault = code.size();
  return_edge(code, Edge::fault);
  for (const auto fixup : fixups) {
    auto target = fault;
    if (fixup.kind == detail::NativeExit::complete) {
      target = fixup.instruction + 1 < starts.size() ? starts[fixup.instruction + 1] : complete;
    }
    code.bind(fixup.displacement, target);
  }
  entry_ = arena_.append(std::move(code).take());
}

Edge Translation::run(detail::NativeContext &context) const noexcept {
  return static_cast<Edge>(cpu12_joined_test_enter(entry_, &context.cpu.registers, &context));
}
} // namespace cpu12::test::joined
