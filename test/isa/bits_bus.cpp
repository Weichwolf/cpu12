// SPDX-License-Identifier: GPL-3.0-only
#include "bits_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>

namespace cpu12::test::bits {
namespace {
struct Port {
  ObservedCore *core = nullptr;
  Registers before{};
  std::uint16_t address = 0x4000;
  Byte data = 0;
  unsigned reads = 0, writes = 0;
  bool accept_read = true, accept_write = true, original = true;

  static void clobber() noexcept {
#if defined(__x86_64__)
    asm volatile("movabs $0xabcdef1234567890, %%r9\n\tstc" : : : "r9", "cc");
#endif
  }

  static bool read(void *owner, const std::uint16_t address, Byte &data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    if (address != port.address) { return false; }
    data = port.data;
    port.original = port.original && port.core->registers() == port.before && port.reads == 0 &&
                    port.writes == 0;
    ++port.reads;
    clobber();
    return port.accept_read;
  }

  static bool write(void *owner, const ByteWrite data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    if (data.address != port.address) { return false; }
    port.data = data.value;
    port.original = port.original && port.core->registers() == port.before && port.reads == 1 &&
                    port.writes == 0;
    ++port.writes;
    clobber();
    return port.accept_write;
  }
};

struct Input {
  Operation operation;
  unsigned mask, data;
  Byte flags;
};

struct Case {
  Input input;
  bool read = true, write = true, partial = false, direct = false;
  unsigned base = 0;
  int delta = 0;
  bool pre = false;
};

struct Capture {
  Code code;
  Point point;
};

Capture capture(Port &port, const Case scene) {
  auto updated = port.before;
  Code code;
  if (scene.delta != 0) {
    constexpr std::array<std::uint16_t Registers::*, 3> fields{
        &Registers::x, &Registers::y, &Registers::sp};
    port.before.*fields[scene.base] =
        static_cast<std::uint16_t>(static_cast<int>(port.address) - (scene.pre ? scene.delta : 0));
    updated = port.before;
    updated.*fields[scene.base] =
        static_cast<std::uint16_t>(static_cast<int>(port.before.*fields[scene.base]) + scene.delta);
    const auto raw = scene.delta > 0 ? static_cast<unsigned>(scene.delta - 1)
                                     : 16U - static_cast<unsigned>(-scene.delta);
    constexpr unsigned auto_mode = 0x20;
    constexpr unsigned post = 0x10;
    code.append(scene.input.operation.indexed);
    code.append(static_cast<Byte>((scene.base << 6U) | auto_mode | (scene.pre ? 0U : post) | raw));
  }
  const Point point{.operation = scene.input.operation,
                    .before = port.before,
                    .updated = updated,
                    .address = port.address,
                    .data = scene.input.data,
                    .mask = scene.input.mask};
  if (scene.delta != 0) {
    tail(code, point);
  } else {
    code = absolute(point, scene.direct);
  }
  return {.code = code, .point = point};
}

void failed(ObservedCore &core, const Capture &captured, const Port &port, const unsigned value) {
  core.set_registers(port.before);
  const auto result = core.run_instruction(captured.code.view());
  const auto fault = port.accept_read ? Fault::memory_write : Fault::memory_read;
  require(result.fault == fault && result.retired == 0 && result.pc == port.before.pc &&
              result.opcode == captured.code.bytes[0] && result.address == port.address &&
              result.value == value && result.access_size == 1 && core.registers() == port.before,
          "Refused bit read/write retains exact context and all original CPU state");
  const auto reads = port.reads;
  const auto writes = port.writes;
  require(core.run_instruction(captured.code.view()).fault == fault && port.reads == reads &&
              port.writes == writes && core.registers() == port.before,
          "Latched bit faults do not replay actual external read/write effects");
}

void one(const Case scene) {
  constexpr std::uint16_t outside = 0x2100;
  constexpr std::uint16_t direct = 0xff;
  Port port{.before = seed,
            .data = static_cast<Byte>(scene.input.data),
            .accept_read = scene.read,
            .accept_write = scene.write};
  port.before.ccr = scene.input.flags;
  if (scene.partial) { port.address = outside; }
  if (scene.direct) { port.address = direct; }
  const auto captured = capture(port, scene);
  std::array<Byte, 256> ram{};
  constexpr std::uint16_t first = 0x2000;
  const auto memory =
      scene.partial ? Memory::window({.first = first, .bytes = ram}) : Memory::external();
  Io io{.owner = &port,
        .read = Port::read,
        .write = scene.input.operation.branch ? nullptr : Port::write};
  ObservedCore core(memory, &io);
  port.core = &core;
  const auto data =
      modified(scene.input.operation, {.data = scene.input.data, .mask = scene.input.mask});
  const bool writes = scene.read && !scene.input.operation.branch;
  if (scene.read && (scene.input.operation.branch || scene.write)) {
    retired(core, captured.code, captured.point);
  } else {
    failed(core, captured, port, scene.read ? data : scene.input.data);
  }
  require(port.reads == 1 && port.writes == static_cast<unsigned>(writes) && port.original,
          "Bit modifications read then write; bit branches only read and every callback sees "
          "original CPU state");
  require(port.data == (writes ? data : scene.input.data),
          "Real bit write effects remain even when their owner refuses the write");
}

void phases(const Input input) {
  for (const bool read : {false, true}) {
    for (const bool write : {false, true}) {
      for (const bool partial : {false, true}) {
        one({.input = input, .read = read, .write = write, .partial = partial});
      }
    }
  }
}

void phase_matrix() {
  constexpr std::array<unsigned, 8> values{0, 1, 0x7f, 0x80, 0xfe, 0xff, 0x55, 0xaa};
  constexpr std::array<Byte, 4> flags{0, 1, 0xfe, 0xff};
  for (const auto operation : operations) {
    for (const auto ccr : flags) {
      for (const auto mask : values) {
        for (const auto data : values) {
          phases({.operation = operation, .mask = mask, .data = data, .flags = ccr});
        }
      }
    }
  }
}

struct Auto {
  unsigned base;
  int delta;
  bool pre;
};

void auto_case(const Auto change) {
  constexpr unsigned mask = 0x80;
  constexpr unsigned all = byte_mask;
  for (const auto operation : operations) {
    for (const bool read : {false, true}) {
      for (const bool write : {false, true}) {
        one({.input = {.operation = operation,
                       .mask = mask,
                       .data = all,
                       .flags = static_cast<Byte>(all)},
             .read = read,
             .write = write,
             .base = change.base,
             .delta = change.delta,
             .pre = change.pre});
      }
    }
  }
}

void auto_cases() {
  for (unsigned base = 0; base < 3; ++base) {
    for (int delta = -8; delta <= 8; ++delta) {
      if (delta == 0) { continue; }
      for (const bool pre : {false, true}) {
        auto_case({.base = base, .delta = delta, .pre = pre});
      }
    }
  }
}

void direct_cases() {
  for (const auto operation : operations) {
    for (const bool read : {false, true}) {
      for (const bool write : {false, true}) {
        one({.input = {.operation = operation, .mask = 0, .data = 0, .flags = 0},
             .read = read,
             .write = write,
             .direct = true});
      }
    }
  }
}
}

void external_cases() {
  phase_matrix();
  auto_cases();
  direct_cases();
}
}
