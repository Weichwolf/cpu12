// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr std::size_t frame_bytes = 9, storage_bytes = frame_bytes + 2;
constexpr Registers seed{
    .pc = 0x0200, .d = 0xa55a, .x = 0x1234, .y = 0x5678, .sp = 0x9000, .ccr = 0x40};
constexpr Address frame = seed.sp - frame_bytes;
constexpr Byte guard = 0xc7;
constexpr std::array<Byte, frame_bytes> expected_frame{
    0x40, 0x5a, 0xa5, 0x12, 0x34, 0x56, 0x78, 0x02, 0x02};

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Input {
  std::size_t leading = 0, size = frame_bytes;
  bool writable = true, known = false, split = false, fails = false;
};

void check(const Input input) {
  std::array<Byte, storage_bytes> bytes{};
  std::array<Byte, storage_bytes> known{};
  bytes.fill(guard);
  auto expected = bytes;
  auto expected_known = known;
  const auto begin = static_cast<Address>(frame - input.leading);
  std::array<Ram, 2> regions{};
  const auto storage = std::span(bytes).first(input.size);
  regions[0] = {.first = begin,
                .bytes = storage,
                .known = input.known ? std::span(known).first(input.size) : std::span<Byte>{},
                .writable = input.writable};
  if (input.split) {
    regions[0].bytes = storage.first(4);
    regions[1] = {.first = frame + 4, .bytes = storage.subspan(4)};
  }
  const auto memory = input.split ? Memory::mapped(regions) : Memory::window(regions[0]);
  Core core(memory);
  core.set_registers(seed);
  const auto result = core.run_instruction(isa::stop);
  auto registers = seed;
  if (!input.fails) {
    registers.pc += isa::stop.size();
    registers.sp = frame;
    for (std::size_t index = 0; index < frame_bytes; ++index) {
      expected[input.leading + index] = expected_frame[index];
      if (input.known) { expected_known[input.leading + index] = 1; }
    }
    require(result.retired == 1 && result.fault == Fault::none &&
                result.state == ExecutionState::stopped,
            "An ordinary complete RAM frame enters STOP");
  } else {
    const auto refused = static_cast<Address>(frame + (input.split ? 3 : 7));
    if (input.split) {
      // PC/Y stores complete first; the X word crosses grants and needs its real owner.
      for (std::size_t index = 5; index < frame_bytes; ++index) {
        expected[index] = expected_frame[index];
      }
    }
    require(result.retired == 0 && result.fault == Fault::memory_write &&
                result.address == refused && result.access_size == 2 &&
                result.state == ExecutionState::running,
            "Short, readonly and split grants preserve the actual word refusal");
    if (input.split) {
      bytes[7] = guard;
      expected[7] = guard;
    }
    require(core.run_instruction(isa::stop) == result,
            "Refused frame stays latched without replaying earlier writes");
  }
  require(core.registers() == registers && bytes == expected && known == expected_known,
          "Frame bytes, both guards, knowledge and all registers match independent expectations");
}

void readonly_code() {
  std::array<Byte, address_bytes> ram{};
  for (std::size_t index = 0; index < isa::stop.size(); ++index) {
    ram[seed.pc + index] = isa::stop[index];
  }
  auto expected = ram;
  for (std::size_t index = 0; index < frame_bytes; ++index) {
    expected[frame + index] = expected_frame[index];
  }
  constexpr Address after = seed.pc + isa::stop.size();
  const std::array regions{Ram{.first = 0, .bytes = std::span(ram).first(seed.pc)},
                           Ram{.first = seed.pc,
                               .bytes = std::span(ram).subspan(seed.pc, isa::stop.size()),
                               .writable = false},
                           Ram{.first = after, .bytes = std::span(ram).subspan(after)}};
  Core core(Memory::mapped(regions));
  core.set_registers(seed);
  const auto result = core.run(1);
  auto registers = seed;
  registers.pc = after;
  registers.sp = frame;
  require(result.retired == 1 && result.fault == Fault::none &&
              result.state == ExecutionState::stopped && core.registers() == registers &&
              ram == expected,
          "Normal immutable-code execution joins the real STOP frame with exact full RAM");
}
} // namespace

int main() {
  try {
    check({});
    check({.leading = 1, .size = storage_bytes});
    check({.size = frame_bytes - 1, .fails = true});
    check({.writable = false, .fails = true});
    check({.known = true});
    check({.split = true, .fails = true});
    readonly_code();
    std::cout << "Native RAM frames preserve minimum spans, guards, permissions, knowledge and "
                 "word faults\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
