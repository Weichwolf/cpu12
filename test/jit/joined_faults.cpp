// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/isa.hpp"
#include "cpu12/memory.hpp"
#include "joined_fixture.hpp"
#include "runtime.hpp"

#include <array>
#include <cstddef>

namespace cpu12::test::joined {
namespace {
constexpr Address port = 0x1234;
constexpr Byte old_value = 0x7f, incremented = 0x80;
constexpr Byte initial_flags = 0xa1;
constexpr Address loaded_result = 0x425a, unqualified_decimal = 0x045a;

struct Bus {
  bool accept_read = true, accept_write = true;
  unsigned reads = 0, writes = 0;
  Byte value = old_value;
  WordWrite attempted{};

  static bool read(void *owner, const Address address, Byte &output) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.reads;
    output = bus.value;
    return address == port && bus.accept_read;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &bus = *static_cast<Bus *>(owner);
    ++bus.writes;
    bus.attempted = {.address = access.address, .value = access.value};
    if (access.address != port || !bus.accept_write) { return false; }
    bus.value = access.value;
    return true;
  }
};

void external_modify(const bool accept_read, const bool accept_write) {
  Bus bus{.accept_read = accept_read, .accept_write = accept_write};
  const Translation translation(
      {instruction<isa::Opcode::inc_indexed>(std::array<Byte, 1>{0}),
       instruction<isa::Opcode::ldaa_immediate>(std::array<Byte, 1>{0x42})},
      Memory::external());
  auto context = fresh_context();
  context.io = {.owner = &bus, .read = &Bus::read, .write = &Bus::write};
  context.cpu.registers.ccr = initial_flags;
  const auto initial = context.cpu;
  const auto edge = translation.run(context);
  require(bus.reads == 1 && bus.writes == (accept_read ? 1U : 0U),
          "Generated RMW makes one real read and only the required real write");
  if (accept_read && accept_write) {
    auto expected = initial;
    expected.registers.pc = 2; // FFFE + INC(2) + LDAA(2).
    expected.registers.d = loaded_result;
    require(edge == Edge::complete && context.fault == Fault::none && context.cpu == expected &&
                bus.value == incremented,
            "Accepted framed RMW joins the next instruction with real memory effects");
  } else {
    require(edge == Edge::fault && context.cpu == initial &&
                context.fault == (accept_read ? Fault::memory_write : Fault::memory_read) &&
                context.address == port && context.access_size == 1 && bus.value == old_value,
            "Framed bus refusal exits without following instruction or register publication");
  }
  if (accept_read) {
    require(bus.attempted.address == port && bus.attempted.value == incremented,
            "Write acceptance and refusal both retain the actual RMW attempt");
  }
}

void invalid_decimal() {
  const Translation translation(
      {instruction<isa::Opcode::daa>(std::array<Byte, 0>{}),
       instruction<isa::Opcode::ldaa_immediate>(std::array<Byte, 1>{0x42})},
      Memory::external());
  auto context = fresh_context();
  context.cpu.registers.d = unqualified_decimal;
  context.cpu.registers.ccr = 0x20; // H=1 requires a low digit <=3; 4 is outside the DAA table.
  const auto initial = context.cpu;
  require(translation.run(context) == Edge::fault && context.cpu == initial &&
              context.fault == Fault::unsupported_state,
          "Unsupported decimal input takes the explicit fault edge before following LDAA");
}
} // namespace

void faults() {
  external_modify(false, false);
  external_modify(true, false);
  external_modify(true, true);
  invalid_decimal();
}

void stop() {
  const auto instruction = isa::stop;
  for (const bool enabled : {false, true}) {
    std::array<Byte, address_bytes> ram{};
    const Translation translation({Encoded(instruction.begin(), instruction.end())},
                                  Memory::unified(ram));
    auto context = fresh_context();
    if (enabled) { context.cpu.registers.ccr = 0x40; }
    auto expected = context.cpu;
    expected.registers.pc = 0;
    auto expected_memory = ram;
    if (enabled) {
      constexpr Address frame = seed.sp - 9;
      expected.registers.sp = frame;
      expected.execution = ExecutionState::stopped;
      // Independent CCR,B,A,Xhi,Xlo,Yhi,Ylo,PChi,PClo frame at decremented SP.
      constexpr std::array<Byte, 9> bytes{0x40, 0x5a, 0xa5, 0x12, 0x34, 0xab, 0xcd, 0, 0};
      for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
        expected_memory[frame + offset] = bytes[offset];
      }
    }
    require(translation.run(context) == Edge::complete && context.fault == Fault::none &&
                context.cpu == expected && ram == expected_memory,
            "Joined STOP preserves enabled sleep or S-masked fallthrough and exact full RAM");
  }
}
} // namespace cpu12::test::joined
