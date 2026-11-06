// SPDX-License-Identifier: GPL-3.0-only
#include "fetch.hpp"

#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "memory_access.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <span>

namespace cpu12::detail {
namespace {
constexpr unsigned prefix = 0x18;

struct Plan {
  unsigned length = 0;
  bool indexed = false;
  bool indirect = false;
};

Plan plan(const unsigned opcode, const bool page) noexcept {
  std::array<Byte, maximum_capture_bytes> bytes{};
  bytes[0] = static_cast<Byte>(page ? prefix : opcode);
  if (page) { bytes[1] = static_cast<Byte>(opcode); }
  for (std::size_t size = page ? 2U : 1U; size <= bytes.size(); ++size) {
    const auto instruction = decode(std::span(bytes).first(size));
    if (!instruction) { continue; }
    const bool excluded = instruction->kind == Kind::effective_address ||
                          instruction->kind == Kind::memory_bits ||
                          instruction->kind == Kind::bit_branch;
    return {.length = instruction->length,
            .indexed = instruction->addressing == Addressing::indexed,
            .indirect = !excluded};
  }
  return {};
}

const Plan &select_plan(const unsigned opcode, const bool page) {
  struct Entry {
    Plan value;
    bool ready = false;
  };

  constexpr std::size_t opcodes = 256;
  // Metadata depends only on the opcode. Populate the execution thread's table
  // on demand instead of decoding both complete opcode pages on first fetch.
  thread_local std::array<Entry, opcodes * 2> entries{};
  auto &entry = entries[opcode + (page ? opcodes : 0U)];
  if (!entry.ready) {
    entry.value = plan(opcode, page);
    entry.ready = true;
  }
  return entry.value;
}

bool byte(Capture &capture, const Memory &ram, NativeContext &context, const Address pc) {
  const auto address = static_cast<Address>(pc + capture.size);
  Byte value = 0;
  if (const auto *pointer = known_byte(ram, address)) {
    value = *pointer;
  } else {
    bool accepted = false;
    if (context.io.fetch != nullptr) {
      accepted = context.io.fetch(context.io.owner, address, value);
    }
    if (!accepted) {
      context.fault = Fault::instruction_fetch;
      context.address = address;
      context.value = value;
      context.access_size = 1;
      context.read_mask = std::numeric_limits<Byte>::max();
      return false;
    }
  }
  capture.bytes[capture.size++] = value;
  return true;
}

unsigned indexed_length(const Plan selected, const unsigned postbyte) {
  constexpr unsigned extended = 0xe0;
  if ((postbyte & extended) != extended) { return selected.length; }
  const auto mode = postbyte & 7U;
  if (!selected.indirect && (mode == 3 || mode == 7)) { return 0; }
  if (mode < 2) { return selected.length + 1; }
  if (mode < 4) { return selected.length + 2; }
  return selected.length;
}
}

Capture fetch(const Memory &ram, NativeContext &context, const Address pc) {
  Capture capture;
  if (!byte(capture, ram, context, pc)) { return capture; }
  const bool page = capture.bytes[0] == prefix;
  if (page && !byte(capture, ram, context, pc)) { return capture; }
  const auto selected = select_plan(capture.bytes[page ? 1U : 0U], page);
  auto length = selected.length;
  if (selected.indexed) {
    if (!byte(capture, ram, context, pc)) { return capture; }
    length = indexed_length(selected, capture.bytes[capture.size - 1]);
  }
  if (length == 0) { return capture; }
  while (capture.size < length) {
    if (!byte(capture, ram, context, pc)) { return capture; }
  }
  capture.supported = true;
  return capture;
}
}
