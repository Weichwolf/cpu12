// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
constexpr Address entry = 0x0200, data = 0x4000, external = 0x6000, stack = 0x9000;
constexpr unsigned byte_bits = 8, byte_mask = 255, word_mask = 65535, sign = 32768;
constexpr unsigned prefix = 8, instructions = 12, frame_bytes = 9, stack_bytes = 16;
constexpr Byte retained = 0xf1, sentinel = 0xa5;
constexpr Address dma_mask = 0x5a5a;
constexpr Registers seed{
    .pc = entry, .d = 0, .x = 0x1357, .y = 0, .sp = stack + stack_bytes, .ccr = 0};
constexpr std::array<unsigned, instructions> offsets{0, 2, 5, 8, 11, 14, 17, 20, 23, 26, 29, 32};
constexpr std::array<Byte, 2> product_high{data >> byte_bits, data &byte_mask};
constexpr std::array<Byte, 2> product_low{data >> byte_bits, (data + 2) & byte_mask};
constexpr std::array<Byte, 2> middle{data >> byte_bits, (data + 1) & byte_mask};
constexpr std::array<Byte, 2> output{data >> byte_bits, (data + 4) & byte_mask};
constexpr std::array<Byte, 2> addend{data >> byte_bits, (data + 6) & byte_mask};
constexpr std::array<Byte, 2> port_address{external >> byte_bits, external &byte_mask};
constexpr auto code = isa::program(isa::emuls,
                                   isa::opcode<isa::Opcode::sty_extended>(),
                                   product_high,
                                   isa::opcode<isa::Opcode::std_extended>(),
                                   product_low,
                                   isa::opcode<isa::Opcode::ldd_extended>(),
                                   middle,
                                   isa::opcode<isa::Opcode::addd_extended>(),
                                   addend,
                                   isa::opcode<isa::Opcode::std_extended>(),
                                   output,
                                   isa::opcode<isa::Opcode::ldd_extended>(),
                                   output,
                                   isa::opcode<isa::Opcode::ldy_extended>(),
                                   output,
                                   isa::opcode<isa::Opcode::ldd_extended>(),
                                   port_address,
                                   isa::opcode<isa::Opcode::addd_extended>(),
                                   output,
                                   isa::opcode<isa::Opcode::std_extended>(),
                                   addend,
                                   isa::stop);

struct Storage {
  std::array<Byte, 8> data{};
  std::array<Byte, stack_bytes> stack{};

  Storage() {
    data.fill(sentinel);
    stack.fill(sentinel);
  }

  bool operator==(const Storage &) const = default;
};

struct Snapshot {
  Registers registers;
  Storage storage;
};

using Trace = std::array<Snapshot, instructions + 1>;

struct Point {
  Address d, y, bias, offered;
  Byte ccr;
};

struct Run {
  Address offered;
  unsigned cap;
  bool accept;
};

void word(std::span<Byte> bytes, const unsigned offset, const Address value) {
  bytes[offset] = static_cast<Byte>(value >> byte_bits);
  bytes[offset + 1U] = static_cast<Byte>(value & byte_mask);
}

int signed_word(const Address value) {
  return value < sign ? int{value} : int{value} - int{word_mask + 1U};
}

void nz(Registers &registers, const Address value) {
  registers.ccr = static_cast<Byte>((registers.ccr & retained) | (value == 0 ? 4U : 0U) |
                                    (value >= sign ? 8U : 0U));
}

void add(Registers &registers, const Address value) {
  const auto before = registers.d;
  const unsigned wide = unsigned{before} + value;
  registers.d = static_cast<Address>(wide & word_mask);
  const bool overflow =
      (before < sign) == (value < sign) && (registers.d < sign) != (before < sign);
  constexpr unsigned preserved = 0xf0;
  registers.ccr = static_cast<Byte>((registers.ccr & preserved) | (wide > word_mask ? 1U : 0U) |
                                    (overflow ? 2U : 0U) | (registers.d == 0 ? 4U : 0U) |
                                    (registers.d >= sign ? 8U : 0U));
}

Trace predict(const Point point) {
  Trace trace{};
  auto current = trace[0];
  current.registers = seed;
  current.registers.d = point.d;
  current.registers.y = point.y;
  current.registers.ccr = point.ccr;
  word(current.storage.data, 6, point.bias);
  trace[0] = current;
  auto &state = current.registers;
  const auto record = [&](const unsigned index, const unsigned length) {
    state.pc = static_cast<Address>(state.pc + length);
    trace[index] = current;
  };
  const auto full = std::int64_t{signed_word(point.d)} * signed_word(point.y);
  const auto bits = static_cast<std::uint32_t>(full);
  state.d = static_cast<Address>(bits & word_mask);
  state.y = static_cast<Address>(bits >> 16U);
  constexpr unsigned multiply_retained = 0xf2;
  constexpr unsigned product_sign_bit = 31;
  state.ccr = static_cast<Byte>((state.ccr & multiply_retained) | ((bits >> 15U) & 1U) |
                                (bits == 0 ? 4U : 0U) | ((bits >> product_sign_bit) * 8U));
  record(1, 2);
  word(current.storage.data, 0, state.y);
  nz(state, state.y);
  record(2, 3);
  word(current.storage.data, 2, state.d);
  nz(state, state.d);
  record(3, 3);
  constexpr std::int64_t scale = 256;
  state.d = static_cast<Address>(full >= 0 ? full / scale : -((-full + scale - 1) / scale));
  nz(state, state.d);
  record(4, 3);
  add(state, point.bias);
  record(5, 3);
  word(current.storage.data, 4, state.d);
  nz(state, state.d);
  record(6, 3);
  nz(state, state.d);
  record(7, 3);
  state.y = state.d;
  nz(state, state.y);
  record(8, 3);
  const auto replacement = static_cast<Address>(point.offered ^ dma_mask);
  word(current.storage.data, 4, replacement);
  state.d = point.offered;
  nz(state, state.d);
  record(9, 3);
  add(state, replacement);
  record(10, 3);
  word(current.storage.data, 6, state.d);
  nz(state, state.d);
  record(11, 3);
  state.pc = static_cast<Address>(state.pc + isa::stop.size());
  state.sp = seed.sp - frame_bytes;
  const std::array<Byte, frame_bytes> frame{state.ccr,
                                            static_cast<Byte>(state.d & byte_mask),
                                            static_cast<Byte>(state.d >> byte_bits),
                                            static_cast<Byte>(state.x >> byte_bits),
                                            static_cast<Byte>(state.x & byte_mask),
                                            static_cast<Byte>(state.y >> byte_bits),
                                            static_cast<Byte>(state.y & byte_mask),
                                            static_cast<Byte>(state.pc >> byte_bits),
                                            static_cast<Byte>(state.pc & byte_mask)};
  std::ranges::copy(frame, current.storage.stack.begin() + (stack_bytes - frame_bytes));
  trace[instructions] = current;
  return trace;
}

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Port {
  const Core *core = nullptr;
  Storage *storage = nullptr;
  Snapshot expected;
  Address offered = 0;
  unsigned calls = 0;
  bool accept = false, observed = true;

  static bool read(void *owner, const Address at, Address &value) noexcept {
    auto &port = *static_cast<Port *>(owner);
    ++port.calls;
    const State expected{.registers = port.expected.registers, .failure = {}};
    port.observed = port.observed && at == external && port.core->state() == expected &&
                    *port.storage == port.expected.storage;
    value = port.offered;
    if (port.accept) { word(port.storage->data, 4, static_cast<Address>(port.offered ^ dma_mask)); }
    alignas(16) std::array<Byte, 16> aligned{};
    // Exercise real aligned stack use across the native-to-owner ABI boundary.
    asm volatile("movaps (%0),%%xmm0" : : "r"(aligned.data()) : "xmm0", "memory");
    asm volatile("xor %%eax,%%eax\n"
                 "xor %%ecx,%%ecx\n"
                 "xor %%edx,%%edx\n"
                 "xor %%esi,%%esi\n"
                 "xor %%edi,%%edi\n"
                 "xor %%r8d,%%r8d\n"
                 "xor %%r9d,%%r9d\n"
                 "xor %%r10d,%%r10d\n"
                 "xor %%r11d,%%r11d\n"
                 "pxor %%xmm1,%%xmm1\n"
                 "pxor %%xmm2,%%xmm2\n"
                 "pxor %%xmm3,%%xmm3\n"
                 "pxor %%xmm4,%%xmm4\n"
                 "pxor %%xmm5,%%xmm5\n"
                 "pxor %%xmm6,%%xmm6\n"
                 "pxor %%xmm7,%%xmm7\n"
                 "pxor %%xmm8,%%xmm8\n"
                 "pxor %%xmm9,%%xmm9\n"
                 "pxor %%xmm10,%%xmm10\n"
                 "pxor %%xmm11,%%xmm11\n"
                 "pxor %%xmm12,%%xmm12\n"
                 "pxor %%xmm13,%%xmm13\n"
                 "pxor %%xmm14,%%xmm14\n"
                 "pxor %%xmm15,%%xmm15\n"
                 :
                 :
                 : "rax",
                   "rcx",
                   "rdx",
                   "rsi",
                   "rdi",
                   "r8",
                   "r9",
                   "r10",
                   "r11",
                   "xmm1",
                   "xmm2",
                   "xmm3",
                   "xmm4",
                   "xmm5",
                   "xmm6",
                   "xmm7",
                   "xmm8",
                   "xmm9",
                   "xmm10",
                   "xmm11",
                   "xmm12",
                   "xmm13",
                   "xmm14",
                   "xmm15",
                   "cc",
                   "memory");
    return port.accept;
  }
};

struct Device {
  std::array<Byte, code.size()> program = code;
  Storage storage;
  Port port;
  Io io{.owner = &port, .read_word = &Port::read};
  std::array<Ram, 3> regions{Ram{.first = entry, .bytes = program, .writable = false},
                             Ram{.first = data, .bytes = storage.data},
                             Ram{.first = stack, .bytes = storage.stack}};
  Core core{Memory::mapped(regions), &io};
};

unsigned scenario(Device &device, const Trace &trace, const Run run) {
  const auto offered = run.offered;
  const auto cap = run.cap;
  const auto accept = run.accept;
  device.storage = trace[0].storage;
  device.core.set_registers(trace[0].registers);
  device.port = {.core = &device.core,
                 .storage = &device.storage,
                 .expected = trace[prefix],
                 .offered = offered,
                 .accept = accept};
  const auto native = device.core.statistics().native_instructions;
  unsigned index = 0;
  unsigned boundaries = 0;
  while (true) {
    const bool failed = !accept && index + cap > prefix;
    const unsigned end = accept ? instructions : prefix;
    const auto count = std::min(cap, end - index);
    index += count;
    const auto result = device.core.run(cap);
    const auto &expected = trace[index];
    require(device.core.registers() == expected.registers && device.storage == expected.storage &&
                device.port.observed && result.retired == count,
            "A connected prefix boundary disagrees with independent registers, flags or RAM");
    RunResult wanted{.retired = count};
    if (failed) {
      wanted.fault = Fault::memory_read;
      wanted.pc = entry + offsets[prefix];
      wanted.opcode = isa::opcode<isa::Opcode::ldd_extended>()[0];
      wanted.address = external;
      wanted.value = offered;
      wanted.access_size = 2;
    } else {
      wanted.pc = static_cast<Address>(entry + offsets[index - 1]);
      wanted.opcode = code[offsets[index - 1]];
      wanted.state = index == instructions ? ExecutionState::stopped : ExecutionState::running;
    }
    if (result != wanted) {
      std::cerr << "cap=" << cap << " index=" << index << " accept=" << accept
                << " pc=" << result.pc << "/" << wanted.pc << " opcode=" << unsigned{result.opcode}
                << "/" << unsigned{wanted.opcode}
                << " fault=" << unsigned{static_cast<Byte>(result.fault)} << "/"
                << unsigned{static_cast<Byte>(wanted.fault)} << " address=" << result.address << "/"
                << wanted.address << " value=" << result.value << "/" << wanted.value
                << " size=" << unsigned{result.access_size} << "/" << unsigned{wanted.access_size}
                << " mask=" << unsigned{result.read_mask} << "/" << unsigned{wanted.read_mask}
                << '\n';
    }
    require(result == wanted,
            "A prefix/suffix exit must retain original retirement and fault metadata");
    State state{.registers = expected.registers, .failure = {}, .execution = wanted.state};
    if (failed) {
      state.failure = wanted;
      state.failure.retired = 0;
    }
    require(device.core.state() == state,
            "Connected execution must publish exact complete Core state");
    ++boundaries;
    if (failed || index == instructions) { break; }
  }
  require(device.port.calls == 1 && device.core.statistics().native_instructions == native + index,
          "The actual owner and native retirement must each be counted exactly once");
  return boundaries;
}
}

int main() {
  try {
    Device device;
    const Point warm{.d = 0, .y = 0, .bias = 0, .offered = 0, .ccr = 0};
    static_cast<void>(scenario(device, predict(warm), {.offered = 0, .cap = 1, .accept = true}));
    const auto compiled = device.core.statistics().compiled_entries;
    constexpr std::array<Address, 5> values{0, 1, 0x7fff, 0x8000, 0xffff};
    constexpr std::array<Address, 3> biases{0, 0x7fff, 0xffff};
    constexpr std::array<unsigned, 8> caps{1, 2, 3, 4, 5, 7, 9, 12};
    unsigned cases = 0;
    unsigned boundaries = 0;
    for (const auto d : values) {
      for (const auto y : values) {
        for (const auto bias : biases) {
          constexpr unsigned stop_disabled = 0x80;
          for (unsigned ccr = 0; ccr < stop_disabled; ++ccr) {
            const auto offered = static_cast<Address>(d ^ y);
            const auto trace = predict(
                {.d = d, .y = y, .bias = bias, .offered = offered, .ccr = static_cast<Byte>(ccr)});
            for (const auto cap : caps) {
              for (const bool accept : {false, true}) {
                boundaries +=
                    scenario(device, trace, {.offered = offered, .cap = cap, .accept = accept});
                ++cases;
              }
            }
          }
        }
      }
    }
    require(device.core.statistics().compiled_entries == compiled,
            "Warm connected prefixes must reuse immutable code across data, flags and grants");
    std::cout << cases << " connected prefix scenarios and " << boundaries
              << " precise RAM/owner/STOP boundaries passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
