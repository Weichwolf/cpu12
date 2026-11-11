// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_callbacks_support.hpp"
#include "image_support.hpp"
#include "isa.hpp"
#include "jit/block.hpp"
#include "jit/llvm/artifact.hpp"
#include "jit/llvm/lifetime.hpp"
#include "public_io.hpp"
#include "runtime.hpp"

#include "llvm-c/Core.h"
#include "llvm-c/IRReader.h"
#include "llvm-c/Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cpu12::test::image {
namespace {
constexpr std::uint64_t id = 7;
constexpr Address input = 0x1234, output = 0x2345, idle = entry + 12;
constexpr Byte retained_flags = 0xf1;
constexpr auto absent = std::numeric_limits<unsigned>::max();
constexpr Registers initial{
    .pc = entry, .d = 0x5aa5, .x = 0x1357, .y = 0x2468, .sp = 0x9000, .ccr = 0};
constexpr std::array<Byte, 2> initial_output{0xff, 0xee};
constexpr std::array callbacks{
    CallbackBinding{.role = CallbackRole::read, .symbol = "fixture_read"},
    CallbackBinding{.role = CallbackRole::write, .symbol = "fixture_write"},
    CallbackBinding{.role = CallbackRole::read_word, .symbol = "fixture_read_word"},
    CallbackBinding{.role = CallbackRole::write_word, .symbol = "fixture_write_word"},
    CallbackBinding{.role = CallbackRole::read_masked, .symbol = "fixture_read_masked"}};
constexpr auto code = isa::program(isa::opcode<isa::Opcode::ldaa_extended>(),
                                   std::array<Byte, 2>{0x12, 0x34},
                                   isa::opcode<isa::Opcode::staa_extended>(),
                                   std::array<Byte, 2>{0x23, 0x45},
                                   isa::opcode<isa::Opcode::ldd_extended>(),
                                   std::array<Byte, 2>{0x12, 0x34},
                                   isa::opcode<isa::Opcode::std_extended>(),
                                   std::array<Byte, 2>{0x23, 0x45},
                                   std::array<Byte, 2>{bra, backedge});
constexpr std::array roots{entry};
const std::array spans{CodeSpan{.address = entry, .words = code}};
const std::array variants{CodeVariant{.id = id, .code = spans, .entries = roots}};
const std::array layout{RegionLayout{.first = entry, .size = code.size(), .writable = false}};
const std::array layouts{VariantLayout{.id = id, .memory = layout}};

std::vector<std::byte> public_bindings() {
  LLVMMemoryBufferRef raw = nullptr;
  char *error = nullptr;
  const auto failed = LLVMCreateMemoryBufferWithContentsOfFile(CPU12_PUBLIC_BINDINGS, &raw, &error);
  const detail::llvm19::Owner<char *, LLVMDisposeMessage> message(error);
  const detail::llvm19::Owner<LLVMMemoryBufferRef, LLVMDisposeMemoryBuffer> buffer(raw);
  require(failed == 0 && buffer != nullptr,
          "Read consumer IR compiled using only public core types");
  const auto *bytes = reinterpret_cast<const std::byte *>(LLVMGetBufferStart(buffer.get()));
  return {bytes, bytes + LLVMGetBufferSize(buffer.get())};
}

struct Point {
  Byte high, low, flags;
  unsigned quota, failure, yielding;
};

struct Expected {
  State state;
  Progress progress;
  public_io::Bus bus;
  unsigned retired = 0;
  Fault fault = Fault::none;
  Address address = 0, value = 0;
  Byte width = 0;
  bool yielded = false;
};

bool expected_access(Expected &result, const Point point) {
  auto &registers = result.state.registers;
  const unsigned operation = (registers.pc - entry) / 3U;
  const bool write = operation % 2 != 0;
  const bool word = operation >= 2;
  const auto address = write ? output : input;
  const std::array<Address, 4> values{
      point.high,
      static_cast<Address>(registers.d >> byte_bits),
      static_cast<Address>((unsigned{point.high} << byte_bits) | point.low),
      registers.d};
  const auto value = values[operation];
  const auto call = result.bus.calls++;
  result.bus.trace[call] = {.address = address,
                            .value = value,
                            .width = static_cast<Byte>(word ? 2 : 1),
                            .write = write,
                            .state = result.state,
                            .progress = result.progress};
  if (write) {
    result.bus.memory[output] = static_cast<Byte>(word ? value >> byte_bits : value);
    if (word) { result.bus.memory[output + 1] = static_cast<Byte>(value); }
  }
  result.yielded = call == point.yielding;
  if (call == point.failure) {
    result.fault = write ? Fault::memory_write : Fault::memory_read;
    result.address = address;
    result.value = value;
    result.width = static_cast<Byte>(word ? 2 : 1);
    return false;
  }
  if (!write) {
    registers.d =
        word ? value
             : static_cast<Address>((unsigned{value} << byte_bits) | (registers.d & byte_mask));
  }
  const unsigned sign = word ? 0x8000 : 0x80;
  registers.ccr = static_cast<Byte>((registers.ccr & retained_flags) |
                                    ((value & sign) != 0 ? 8U : 0U) | (value == 0 ? 4U : 0U));
  registers.pc += 3;
  ++result.retired;
  result.progress.estimated_cycles += 3;
  return !result.yielded;
}

Expected predict(const Point point, const public_io::Bus &bus) {
  Expected result{.state = {.registers = initial, .failure = {}}, .progress = {}, .bus = bus};
  result.state.registers.ccr = point.flags;
  while (result.retired < point.quota) {
    if (result.state.registers.pc == idle) {
      ++result.retired;
      result.progress.estimated_cycles += 3;
    } else if (!expected_access(result, point)) {
      break;
    }
  }
  return result;
}

struct Yield {
  detail::NativeContext *context;
  const public_io::Bus *bus;
  unsigned call;

  static void notify(void *owner) noexcept {
    const auto &self = *static_cast<const Yield *>(owner);
    if (self.bus->calls == self.call) { self.context->yield_requested = true; }
  }
};

void prefix(const ImageProgram &image, const Point point) {
  auto executable = code;
  const std::array grants{Ram{.first = entry, .bytes = executable, .writable = false}};
  Owner owner(image, id, Memory::mapped(grants));
  auto registers = initial;
  registers.ccr = point.flags;
  owner.seed(registers);
  public_io::Bus bus;
  bus.memory[input] = point.high;
  bus.memory[input + 1] = point.low;
  bus.memory[output] = initial_output[0];
  bus.memory[output + 1] = initial_output[1];
  bus.fail_at = point.failure;
  const auto expected = predict(point, bus);
  Yield yield{.context = &owner.context, .bus = &bus, .call = point.yielding};
  bus.state = &owner.context.cpu;
  bus.progress = &owner.context.progress;
  bus.observer = &yield;
  bus.notify = Yield::notify;
  owner.context.io = {.owner = &bus};
  const auto run = owner.run(point.quota);
  require(
      run.retired == expected.retired && owner.context.cpu == expected.state &&
          owner.context.progress == expected.progress && owner.context.fault == expected.fault &&
          owner.context.address == expected.address && owner.context.value == expected.value &&
          owner.context.access_size == expected.width && owner.context.read_mask == byte_mask &&
          owner.context.yield_requested == expected.yielded && bus.calls == expected.bus.calls &&
          bus.memory == expected.bus.memory && bus.trace == expected.bus.trace,
      "Public-only fused I/O preserves prefixes, live state, partial failures, yield and RAM");
}

void rejects(const std::span<const std::byte> bindings,
             const std::span<const CallbackBinding> declarations,
             const std::string_view reason) {
  bool rejected = false;
  std::string observed = "no rejection";
  try {
    static_cast<void>(
        ImageProgram({.variants = variants}, layouts, bindings, {}, {}, declarations));
  } catch (const std::invalid_argument &error) {
    observed = error.what();
    rejected = std::string_view(error.what()).contains(reason);
  }
  if (!rejected) {
    throw std::runtime_error("Invalid public callback: expected=" + std::string(reason) +
                             "; observed=" + observed);
  }
}

void rejected(const std::span<const std::byte> bindings) {
  const std::array<std::pair<CallbackBinding, std::string_view>, 6> invalid{
      {{{.role = CallbackRole::read, .symbol = "fixture_write"}, "ABI mismatch"},
       {{.role = CallbackRole::read, .symbol = "fixture_read_word"}, "pointer guarantees"},
       {{.role = CallbackRole::read, .symbol = "missing"}, "definition missing"},
       {{.role = CallbackRole::read, .symbol = ""}, "Invalid or duplicate"},
       {{.role = static_cast<CallbackRole>(byte_mask), .symbol = "fixture_read"}, "Unknown"},
       {{.role = CallbackRole::read, .symbol = "cpu12_read_byte"}, "core-owned adapter"}}};
  for (const auto &[declaration, reason] : invalid) {
    const std::array declarations{declaration};
    rejects(bindings, declarations, reason);
  }
  const std::array duplicate{callbacks[0], callbacks[0]};
  rejects(bindings, duplicate, "Invalid or duplicate");
}

std::vector<std::byte> binding_ir(const std::string_view text) {
  const detail::llvm19::Owner<LLVMContextRef, LLVMContextDispose> context(LLVMContextCreate());
  auto *buffer = LLVMCreateMemoryBufferWithMemoryRangeCopy(text.data(), text.size(), "callback");
  LLVMModuleRef raw = nullptr;
  char *error = nullptr;
  const auto failed = LLVMParseIRInContext(context.get(), buffer, &raw, &error);
  const detail::llvm19::Owner<char *, LLVMDisposeMessage> message(error);
  const detail::llvm19::Owner<LLVMModuleRef, LLVMDisposeModule> module(raw);
  require(failed == 0 && module != nullptr,
          "Invalid callback contract fixtures remain valid LLVM IR");
  return detail::llvm19::bitcode(module.get());
}

void rejected_abi() {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 7> invalid{
      {{"define fastcc zeroext i1 @bad(ptr %o, i16 zeroext %a, ptr %v) nounwind { ret i1 true }",
        "ABI mismatch"},
       {"define i1 @bad(ptr %o, i16 zeroext %a, ptr %v) nounwind { ret i1 true }",
        "ABI attribute mismatch"},
       {"define zeroext i1 @bad(ptr %o, i16 zeroext %a, ptr %v) { ret i1 true }", "ABI mismatch"},
       {"define zeroext i1 @bad(ptr noalias %o, i16 zeroext %a, ptr %v) nounwind { ret i1 true }",
        "stronger parameter contract"},
       {"define zeroext i1 @bad(ptr %o, i16 zeroext %a, ptr align 2 dereferenceable(2) %v) "
        "nounwind { ret i1 true }",
        "stronger pointer guarantees"},
       {"declare zeroext i1 @bad(ptr, i16 zeroext, ptr) nounwind", "definition missing"},
       {"define zeroext i1 @bad(ptr %o, i16 zeroext %a, ptr %v, ...) nounwind { ret i1 true }",
        "definition missing"}}};
  constexpr std::array declarations{CallbackBinding{.role = CallbackRole::read, .symbol = "bad"}};
  for (const auto &[text, reason] : invalid) { rejects(binding_ir(text), declarations, reason); }
}

void unspecified_layout() {
  const auto bitcode =
      binding_ir("define zeroext i1 @manual_read(ptr %o, i16 zeroext %a, ptr %v) nounwind { "
                 "store i8 90, ptr %v\nret i1 false }");
  constexpr std::array declarations{
      CallbackBinding{.role = CallbackRole::read, .symbol = "manual_read"}};
  const ImageProgram image({.variants = variants}, layouts, bitcode, {}, {}, declarations);
  auto executable = code;
  const std::array grants{Ram{.first = entry, .bytes = executable, .writable = false}};
  Owner owner(image, id, Memory::mapped(grants));
  owner.seed(initial);
  constexpr Address partial = 90;
  require(
      owner.run(1).retired == 0 &&
          owner.context.cpu == State{.registers = initial, .failure = {}} &&
          owner.context.progress == Progress{} && owner.context.fault == Fault::memory_read &&
          owner.context.address == input && owner.context.value == partial &&
          owner.context.access_size == 1 && owner.context.read_mask == byte_mask &&
          executable == code,
      "Target-neutral scalar binding IR adopts the checked native layout and preserves failure");
}
}

void callbacks_contract() {
  const auto bindings = public_bindings();
  rejected(bindings);
  rejected_abi();
  unspecified_layout();
  constexpr std::array requests{InterruptRequest{.vector = 0xfff0}};
  const ImageProgram image({.variants = variants}, layouts, bindings, requests, {}, callbacks);
  callback_events(image);
  callback_masked(bindings, callbacks);
  constexpr std::array<std::array<Byte, 2>, 4> values{
      {{0, 0}, {0x80, 0x12}, {0x7f, 0xff}, {0, 0x24}}};
  constexpr std::array<unsigned, 5> boundaries{0, 1, 2, 3, absent};
  constexpr std::array<unsigned, 7> quotas{0, 1, 2, 3, 4, 5, 8};
  for (const auto value : values) {
    for (unsigned flags = 0; flags <= byte_mask; ++flags) {
      for (const auto failure : boundaries) {
        for (const auto yielding : boundaries) {
          for (const auto quota : quotas) {
            prefix(image,
                   {.high = value[0],
                    .low = value[1],
                    .flags = static_cast<Byte>(flags),
                    .quota = quota,
                    .failure = failure,
                    .yielding = yielding});
          }
        }
      }
    }
  }
}
}
