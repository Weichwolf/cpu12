// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_support.hpp"
#include "isa.hpp"
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/lifetime.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/IRReader.h"
#include "llvm-c/Object.h"
#include "llvm-c/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace cpu12::test::image {
namespace {
template <class Ref, auto Dispose> using LlvmOwner = detail::llvm19::Owner<Ref, Dispose>;
constexpr std::uint64_t id = 7;
constexpr Address input = 0x1234, output = 0x2345;
constexpr Registers initial{
    .pc = entry, .d = 0x5aa5, .x = 0x1357, .y = 0x2468, .sp = 0x9000, .ccr = 0xc1};
constexpr std::array<std::pair<Byte, Byte>, 4> samples{
    {{0x80, 0x12}, {0, 0}, {0x7f, 0xff}, {0x42, 0x24}}};
constexpr std::array<Byte, 14> bytes = isa::program(isa::opcode<isa::Opcode::ldaa_extended>(),
                                                    std::array<Byte, 2>{0x12, 0x34},
                                                    isa::opcode<isa::Opcode::staa_extended>(),
                                                    std::array<Byte, 2>{0x23, 0x45},
                                                    isa::opcode<isa::Opcode::ldd_extended>(),
                                                    std::array<Byte, 2>{0x12, 0x34},
                                                    isa::opcode<isa::Opcode::std_extended>(),
                                                    std::array<Byte, 2>{0x23, 0x45},
                                                    std::array<Byte, 2>{bra, backedge});
constexpr std::array layout{RegionLayout{.first = entry, .size = bytes.size(), .writable = false}};
constexpr std::array<Address, 1> entries{entry};
const std::array spans{CodeSpan{.address = entry, .words = bytes}};
const std::array variants{CodeVariant{.id = id, .code = spans, .entries = entries}};
const std::array layouts{VariantLayout{.id = id, .memory = layout}};

std::vector<std::byte> binding_file() {
  char *error = nullptr;
  LLVMMemoryBufferRef raw = nullptr;
  const auto failed = LLVMCreateMemoryBufferWithContentsOfFile(CPU12_IMAGE_BINDINGS, &raw, &error);
  const LlvmOwner<char *, LLVMDisposeMessage> message(error);
  const LlvmOwner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> buffer(raw);
  require(failed == 0 && buffer != nullptr, "Read independently compiled C++ binding bitcode");
  const auto *first = reinterpret_cast<const std::byte *>(LLVMGetBufferStart(buffer.get()));
  return {first, first + LLVMGetBufferSize(buffer.get())};
}

std::vector<std::byte> binding(const std::string_view text) {
  const LlvmOwner<LLVMContextRef, LLVMContextDispose> context(LLVMContextCreate());
  auto *buffer = LLVMCreateMemoryBufferWithMemoryRangeCopy(text.data(), text.size(), "binding");
  LLVMModuleRef raw = nullptr;
  char *error = nullptr;
  const auto failed = LLVMParseIRInContext(context.get(), buffer, &raw, &error);
  const LlvmOwner<char *, LLVMDisposeMessage> message(error);
  const LlvmOwner<LLVMModuleRef, LLVMDisposeModule> module(raw);
  require(failed == 0 && module != nullptr, "Binding rejection fixtures must be valid LLVM IR");
  return detail::llvm19::bitcode(module.get());
}

void no_call_relocations(const std::span<const std::byte> object) {
  const LlvmOwner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> buffer(
      LLVMCreateMemoryBufferWithMemoryRangeCopy(
          reinterpret_cast<const char *>(object.data()), object.size(), "image"));
  char *error = nullptr;
  const LlvmOwner<LLVMBinaryRef, LLVMDisposeBinary> binary(
      LLVMCreateBinary(buffer.get(), nullptr, &error));
  const LlvmOwner<char *, LLVMDisposeMessage> message(error);
  require(binary != nullptr, "Image export must be a real native object");
  const LlvmOwner<LLVMSymbolIteratorRef, LLVMDisposeSymbolIterator> symbol(
      LLVMObjectFileCopySymbolIterator(binary.get()));
  require(symbol != nullptr, "Native object must retain callable generated functions");
  while (LLVMObjectFileIsSymbolIteratorAtEnd(binary.get(), symbol.get()) == 0) {
    const auto *name = LLVMGetSymbolName(symbol.get());
    require(name == nullptr || (!std::string_view(name).contains("cpu12_read_") &&
                                !std::string_view(name).contains("cpu12_write_")),
            "LLVM O3 must fuse these small bindings into the emitted native object");
    LLVMMoveToNextSymbol(symbol.get());
  }
}

std::pair<Registers, Progress> predict(const unsigned budget, const Byte high, const Byte low) {
  auto expected = initial;
  std::uint64_t cycles = 0;
  if (budget >= 1) {
    expected.d = static_cast<Address>((unsigned{high} << byte_bits) | (initial.d & byte_mask));
    expected.pc += 3;
    cycles += 3;
  }
  if (budget >= 2) {
    expected.pc += 3;
    cycles += 3;
  }
  if (budget >= 3) {
    expected.d = static_cast<Address>((unsigned{high} << byte_bits) | low);
    expected.pc += 3;
    cycles += 3;
  }
  if (budget >= 4) {
    expected.pc += 3;
    cycles += 3;
  }
  if (budget >= 1) {
    const bool zero = budget < 3 ? high == 0 : expected.d == 0;
    expected.ccr = static_cast<Byte>(initial.ccr | (high >= 0x80 ? 8U : 0U) | (zero ? 4U : 0U));
  }
  if (budget >= 5) { cycles += std::uint64_t{budget - 4} * 3; }
  return {expected, {.estimated_cycles = cycles}};
}

void binding_execution(const ImageProgram &image, const std::pair<Byte, Byte> sample) {
  const auto [high, low] = sample;
  std::array<Byte, address_bytes> data{};
  data[input] = high;
  data[input + 1] = low;
  auto code = bytes;
  const std::array grants{Ram{.first = entry, .bytes = code, .writable = false}};
  Owner owner(image, id, Memory::mapped(grants));
  for (unsigned budget = 0; budget <= 8; ++budget) {
    owner.seed(initial);
    owner.context.io.owner = data.data();
    data[output] = 0;
    data[output + 1] = 0;
    const auto [registers, progress] = predict(budget, high, low);
    auto wanted = data;
    if (budget >= 2) { wanted[output] = high; }
    if (budget >= 4) { wanted[output + 1] = low; }
    require(owner.run(budget).retired == budget &&
                owner.context.cpu == State{.registers = registers, .failure = {}} &&
                owner.context.progress == progress && data == wanted,
            "Fused C++ bindings preserve independent full state, exact prefixes and owner memory");
  }
}

enum class BitSite : std::uint8_t {
  set = 0,
  select = 4,
  clear_one = 9,
  skip = 13,
  clear_two = 15,
  clear_branch = 19,
  set_one = 24,
  idle = 28
};

struct BitResult {
  Registers registers = initial;
  std::uint64_t cycles = 0;
  Byte data;
};

struct BitInput {
  unsigned budget;
  Byte data;
};

BitResult bit_prefix(const BitInput point) {
  BitResult result{.data = point.data};
  auto pc = BitSite::set;
  constexpr unsigned select = 0x80;
  constexpr unsigned set = 0x40;
  constexpr unsigned condition = 8;
  constexpr unsigned retained = 0xf1;
  for (unsigned retired = 0; retired < point.budget; ++retired) {
    bool changed = false;
    unsigned cycles = 4;
    switch (pc) {
      case BitSite::set:
        result.data |= set;
        pc = BitSite::select;
        changed = true;
        break;
      case BitSite::select:
        pc = (result.data & select) != 0 ? BitSite::clear_two : BitSite::clear_one;
        cycles = 5;
        break;
      case BitSite::clear_one:
        result.data &= byte_mask ^ 1U;
        pc = BitSite::skip;
        changed = true;
        break;
      case BitSite::skip:
        pc = BitSite::clear_branch;
        cycles = 3;
        break;
      case BitSite::clear_two:
        result.data &= byte_mask ^ 2U;
        pc = BitSite::clear_branch;
        changed = true;
        break;
      case BitSite::clear_branch:
        pc = (result.data & condition) == 0 ? BitSite::idle : BitSite::set_one;
        cycles = 5;
        break;
      case BitSite::set_one:
        result.data |= 1;
        pc = BitSite::idle;
        changed = true;
        break;
      case BitSite::idle: cycles = 3; break;
      default: throw std::logic_error("Invalid independent bit prefix");
    }
    if (changed) {
      result.registers.ccr =
          static_cast<Byte>((result.registers.ccr & retained) | (result.data >= select ? 8U : 0U) |
                            (result.data == 0 ? 4U : 0U));
    }
    result.cycles += cycles;
  }
  result.registers.pc = static_cast<Address>(entry + static_cast<Address>(pc));
  return result;
}

void bit_artifacts(const std::span<const std::byte> bindings) {
  const auto byte = [](const isa::Opcode operation) { return static_cast<Byte>(operation); };
  const std::array<Byte, 30> code{byte(isa::Opcode::bset_extended),
                                  0x12,
                                  0x34,
                                  0x40,
                                  byte(isa::Opcode::brset_extended),
                                  0x12,
                                  0x34,
                                  0x80,
                                  6,
                                  byte(isa::Opcode::bclr_extended),
                                  0x12,
                                  0x34,
                                  1,
                                  bra,
                                  4,
                                  byte(isa::Opcode::bclr_extended),
                                  0x12,
                                  0x34,
                                  2,
                                  byte(isa::Opcode::brclr_extended),
                                  0x12,
                                  0x34,
                                  8,
                                  4,
                                  byte(isa::Opcode::bset_extended),
                                  0x12,
                                  0x34,
                                  1,
                                  bra,
                                  backedge};
  const std::array source{CodeSpan{.address = entry, .words = code}};
  const std::array declarations{CodeVariant{.id = id, .code = source, .entries = entries}};
  const std::array regions{RegionLayout{.first = entry, .size = code.size(), .writable = false}};
  const std::array memory{VariantLayout{.id = id, .memory = regions}};
  const ImageProgram image({.variants = declarations}, memory, bindings);
  no_call_relocations(image.program(id).object());
  auto executable = code;
  Owner owner(image, id, Memory::window({.first = entry, .bytes = executable, .writable = false}));
  for (unsigned data = 0; data <= byte_mask; ++data) {
    for (unsigned budget = 0; budget <= 12; ++budget) {
      std::array<Byte, address_bytes> ram{};
      ram[input] = static_cast<Byte>(data);
      auto wanted = ram;
      const auto result = bit_prefix({.budget = budget, .data = static_cast<Byte>(data)});
      wanted[input] = result.data;
      owner.seed(initial);
      owner.context.io.owner = ram.data();
      require(owner.run(budget).retired == budget &&
                  owner.context.cpu == State{.registers = result.registers, .failure = {}} &&
                  owner.context.progress.estimated_cycles == result.cycles && ram == wanted,
              "Fused C++ selected-byte bindings preserve actual bit branches, memory, flags and "
              "prefixes");
    }
  }
  const auto invalid =
      binding("define i64 @cpu12_read_masked_byte(ptr %p, i16 %a, i8 %m) { ret i64 0 }");
  bool rejected = false;
  try {
    static_cast<void>(ImageProgram({.variants = declarations}, memory, invalid));
  } catch (const std::invalid_argument &error) {
    rejected = std::string_view(error.what()).contains("ABI mismatch");
  }
  require(rejected, "Masked-byte bindings reject incompatible ABI before installation");
}

void move_artifacts(const std::span<const std::byte> bindings) {
  constexpr Byte post_one = 0x30;
  constexpr Byte pre_one = 0x20;
  const auto code = isa::program(isa::opcode<isa::Opcode::movw_indexed_to_indexed>(),
                                 std::array<Byte, 2>{0, 1},
                                 isa::opcode<isa::Opcode::movw_indexed_to_indexed>(),
                                 std::array<Byte, 2>{post_one, pre_one},
                                 std::array<Byte, 2>{bra, backedge});
  const std::array source{CodeSpan{.address = entry, .words = code}};
  const std::array declarations{CodeVariant{.id = id, .code = source, .entries = entries}};
  const std::array regions{RegionLayout{.first = entry, .size = code.size(), .writable = false}};
  const std::array memory{VariantLayout{.id = id, .memory = regions}};
  const ImageProgram image({.variants = declarations}, memory, bindings);
  no_call_relocations(image.program(id).object());
  auto executable = code;
  Owner owner(image, id, Memory::window({.first = entry, .bytes = executable, .writable = false}));
  for (unsigned data = 0; data <= byte_mask; ++data) {
    for (unsigned budget = 0; budget <= 6; ++budget) {
      std::array<Byte, address_bytes> ram{};
      ram[input] = static_cast<Byte>(data);
      constexpr Byte other = 0xa5;
      ram[input + 1] = static_cast<Byte>(data ^ other);
      auto wanted = ram;
      auto before = initial;
      before.x = input;
      auto registers = before;
      std::uint64_t cycles = 0;
      if (budget >= 1) {
        wanted[input + 1] = static_cast<Byte>(data);
        wanted[input + 2] = static_cast<Byte>(data ^ other);
        registers.pc += 4;
        cycles += 5;
      }
      if (budget >= 2) {
        wanted[input + 2] = static_cast<Byte>(data);
        wanted[input + 3] = static_cast<Byte>(data);
        registers.pc += 4;
        registers.x += 2;
        cycles += 5;
      }
      if (budget > 2) { cycles += std::uint64_t{budget - 2} * 3; }
      owner.seed(before);
      owner.context.io.owner = ram.data();
      require(owner.run(budget).retired == budget &&
                  owner.context.cpu == State{.registers = registers, .failure = {}} &&
                  owner.context.progress.estimated_cycles == cycles && ram == wanted,
              "Fused C++ MOV word bindings preserve overlapping source snapshots, sequential "
              "shared indices and exact native prefixes");
    }
  }
}

void rejected_bindings() {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 9> inputs{
      {{"define i64 @cpu12_read_byte(ptr %p, i16 %a) { ret i64 0 }", "ABI mismatch"},
       {"target datalayout = \"e-p:32:32\"\n", "data layout"},
       {"@state = global i32 0\n", "state must be supplied"},
       {"target triple = \"riscv64-unknown-linux-gnu\"\n", "target is incompatible"},
       {"define void @f() #0 { ret void }\nattributes #0 = { \"target-cpu\"=\"cpu12-nonexistent\" "
        "}",
        "unknown target CPU"},
       {"@address = constant ptr inttoptr (i64 4660 to ptr)\n", "absolute host pointer"},
       {"define ptr @f() { %p = inttoptr i64 4660 to ptr\n ret ptr %p }\n",
        "absolute host pointer"},
       {"define void @f() #0 { ret void }\nattributes #0 = { "
        "\"target-features\"=\"+cpu12-nonexistent\" }",
        "unsupported target feature"},
       {"@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 1, "
        "ptr @f, ptr null }]\ndefine void @f() { ret void }",
        "explicit initialization"}}};
  for (const auto &[text, reason] : inputs) {
    const auto bitcode = binding(text);
    bool rejected = false;
    try {
      static_cast<void>(ImageProgram({.variants = variants}, layouts, bitcode));
    } catch (const std::invalid_argument &error) {
      rejected = std::string_view(error.what()).contains(reason);
    }
    require(rejected, "Invalid host IR must fail for its specified contract violation");
  }
  constexpr std::array malformed{
      std::byte{0x42}, std::byte{0x43}, std::byte{0xc0}, std::byte{0xde}};
  bool rejected = false;
  try {
    static_cast<void>(ImageProgram({.variants = variants}, layouts, malformed));
  } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected, "Malformed binding bitcode must fail explicitly before image installation");
}
}

void artifact_contract() {
  const auto bindings = binding_file();
  const ImageProgram image({.variants = variants}, layouts, bindings);
  const auto &program = image.program(id);
  require(!program.bitcode().empty() && !program.object().empty() && program.native_bytes() != 0,
          "The same compiler must retain semantic bitcode and the installed optimized object");
  no_call_relocations(program.object());
  for (const auto sample : std::span(samples).first(3)) { binding_execution(image, sample); }
  rejected_bindings();
  bit_artifacts(bindings);
  move_artifacts(bindings);
  binding_execution(image, samples.back());
}
}
