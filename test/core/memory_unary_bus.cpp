// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "memory_unary_model.hpp"
#include "observed_core.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace cpu12::test::memory_unary {
namespace {
enum class Request : std::uint8_t { pointer, read, write };

struct Event {
  Request kind;
  std::uint16_t address, data;
};

struct Acceptance {
  bool pointer = true, read = true, write = true;
};

struct Port {
  ObservedCore *core = nullptr;
  Registers before{};
  std::array<Byte, memory_bytes> ram{};
  std::array<Event, 4> events{};
  std::uint16_t pointer = static_cast<std::uint16_t>(memory_bytes - 1U), operand = 0x4000;
  Acceptance accept;
  unsigned used = 0;
  bool original = true;

  void observe(const Event event) noexcept {
    original = original && core->registers() == before && used < events.size();
    if (used < events.size()) { events[used] = event; }
    ++used;
  }

  static void clobber() noexcept {
#if defined(__x86_64__)
    asm volatile("movabs $0xabcdef1234567890, %%r9\n\tstc" : : : "r9", "cc");
#endif
  }

  static bool read_word(void *owner, const std::uint16_t address, std::uint16_t &data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    if (address != port.pointer) { return false; }
    const auto next = static_cast<std::uint16_t>(address + 1U);
    data = static_cast<std::uint16_t>((static_cast<unsigned>(port.ram[address]) << byte_bits) |
                                      port.ram[next]);
    port.observe({.kind = Request::pointer, .address = address, .data = data});
    clobber();
    return port.accept.pointer;
  }

  static bool read_byte(void *owner, const std::uint16_t address, Byte &data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    if (address != port.operand) { return false; }
    data = port.ram[address];
    port.observe({.kind = Request::read, .address = address, .data = data});
    clobber();
    return port.accept.read;
  }

  static bool write_byte(void *owner, const ByteWrite data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    if (data.address != port.operand) { return false; }
    port.ram[data.address] = data.value;
    port.observe({.kind = Request::write, .address = data.address, .data = data.value});
    clobber();
    return port.accept.write;
  }

  [[nodiscard]] Io io(const Operation operation) noexcept {
    return {.owner = this,
            .read = operation.read ? read_byte : nullptr,
            .write = operation.write ? write_byte : nullptr,
            .read_word = read_word};
  }
};

struct Case {
  Operation operation;
  Acceptance accept;
  unsigned data, flags;
  bool indirect = false, partial = false, alias = false;
  unsigned base = 0;
  int delta = 0;
  bool pre = false;
};

struct Capture {
  std::array<Byte, 4> code;
  unsigned length;
  Registers updated;
};

Capture capture(Port &port, const Case point) {
  auto updated = port.before;
  if (point.indirect) {
    port.before.x = port.pointer;
    updated = port.before;
    constexpr Byte indirect_x = 0xe3;
    return {.code = {point.operation.indexed, indirect_x, 0, 0}, .length = 4, .updated = updated};
  }
  if (point.delta != 0) {
    constexpr std::array<std::uint16_t Registers::*, 3> fields{
        &Registers::x, &Registers::y, &Registers::sp};
    port.before.*fields[point.base] =
        static_cast<std::uint16_t>(static_cast<int>(port.operand) - (point.pre ? point.delta : 0));
    updated = port.before;
    updated.*fields[point.base] =
        static_cast<std::uint16_t>(static_cast<int>(port.before.*fields[point.base]) + point.delta);
    const auto raw = point.delta > 0 ? static_cast<unsigned>(point.delta - 1)
                                     : 16U - static_cast<unsigned>(-point.delta);
    constexpr unsigned auto_mode = 0x20;
    constexpr unsigned post = 0x10;
    const auto postbyte =
        static_cast<Byte>((point.base << 6U) | auto_mode | (point.pre ? 0U : post) | raw);
    return {.code = {point.operation.indexed, postbyte, 0, 0}, .length = 2, .updated = updated};
  }
  return {.code = {point.operation.extended,
                   static_cast<Byte>(port.operand >> byte_bits),
                   static_cast<Byte>(port.operand),
                   0},
          .length = 3,
          .updated = updated};
}

struct Contract {
  RunResult fault;
  std::array<Request, 3> requests{};
  unsigned count = 0;
  bool writes = false;
};

struct Values {
  unsigned original, result;
};

Contract contract(const Port &port, const Case point, const Values data) {
  Contract wanted;
  if (point.indirect) {
    wanted.requests[wanted.count++] = Request::pointer;
    if (!point.accept.pointer) {
      wanted.fault = {.fault = Fault::memory_read,
                      .address = port.pointer,
                      .value = port.operand,
                      .access_size = 2};
      return wanted;
    }
  }
  if (point.operation.read) {
    wanted.requests[wanted.count++] = Request::read;
    if (!point.accept.read) {
      wanted.fault = {.fault = Fault::memory_read,
                      .address = port.operand,
                      .value = static_cast<std::uint16_t>(data.original),
                      .access_size = 1};
      return wanted;
    }
  }
  if (point.operation.write) {
    wanted.requests[wanted.count++] = Request::write;
    wanted.writes = true;
    if (!point.accept.write) {
      wanted.fault = {.fault = Fault::memory_write,
                      .address = port.operand,
                      .value = static_cast<std::uint16_t>(data.result),
                      .access_size = 1};
    }
  }
  return wanted;
}

void check_failure(ObservedCore &core,
                   const std::span<const Byte> code,
                   const Port &port,
                   const Contract wanted) {
  const auto result = core.run_instruction(code);
  require(
      result.retired == 0 && result.fault == wanted.fault.fault && result.pc == port.before.pc &&
          result.opcode == code[0] && result.address == wanted.fault.address &&
          result.value == wanted.fault.value && result.access_size == wanted.fault.access_size &&
          core.registers() == port.before,
      "Refused unary pointer/read/write phase retains exact context and all original CPU state");
  const auto stats = core.statistics();
  const auto count = port.used;
  require(core.run_instruction(code).retired == 0 && port.used == count &&
              core.statistics().read_callbacks == stats.read_callbacks &&
              core.statistics().write_callbacks == stats.write_callbacks,
          "Latched unary phase faults never replay pointer reads, operand reads or actual writes");
}

void one(const Case point) {
  Port port{.before = seed, .accept = point.accept};
  port.before.ccr = static_cast<Byte>(point.flags);
  constexpr std::uint16_t outside_ram = 0x2100;
  if (point.partial) { port.operand = outside_ram; }
  if (point.alias) { port.pointer = port.operand; }
  const auto captured = capture(port, point);
  port.ram[port.pointer] = static_cast<Byte>(port.operand >> byte_bits);
  port.ram[static_cast<std::uint16_t>(port.pointer + 1U)] = static_cast<Byte>(port.operand);
  if (!point.alias) { port.ram[port.operand] = static_cast<Byte>(point.data); }
  const auto original = port.ram[port.operand];
  const auto predicted = prediction(point.operation, port.before, original);
  const auto wanted = contract(port, point, {.original = original, .result = predicted.data});
  auto io = port.io(point.operation);
  constexpr std::uint16_t ram_start = 0x2000;
  constexpr std::uint16_t ram_size = 0x100;
  const auto memory =
      point.partial ? Memory::window({.first = ram_start,
                                      .bytes = std::span(port.ram).subspan(ram_start, ram_size)})
                    : Memory::external();
  ObservedCore core(memory, &io);
  port.core = &core;
  core.set_registers(port.before);
  const auto code = std::span(captured.code).first(captured.length);
  if (wanted.fault.fault == Fault::none) {
    retired(core, code, port.before, expected(port.before, captured.updated, code, predicted));
  } else {
    check_failure(core, code, port, wanted);
  }
  require(port.used == wanted.count && port.original,
          "Every external unary callback observes the original unretired registers and flags");
  for (unsigned i = 0; i < wanted.count; ++i) {
    require(port.events[i].kind == wanted.requests[i] &&
                port.events[i].address ==
                    (wanted.requests[i] == Request::pointer ? port.pointer : port.operand),
            "Unary external requests preserve actual pointer/read/write order and target");
  }
  require(port.ram[port.operand] == (wanted.writes ? predicted.data : original),
          "Accepted and refused writes retain their real byte effect without host rollback");
}

void phase_matrix() {
  constexpr std::array<unsigned, 4> flags{0, 1, 0xfe, 0xff};
  constexpr std::array<unsigned, 8> values{0, 1, 0x7f, 0x80, 0xfe, 0xff, 0x10, 0x55};
  for (const auto operation : operations) {
    for (const auto ccr : flags) {
      for (const auto data : values) {
        for (const bool read : {false, true}) {
          for (const bool write : {false, true}) {
            one({.operation = operation,
                 .accept = {.read = read, .write = write},
                 .data = data,
                 .flags = ccr});
            one({.operation = operation,
                 .accept = {.read = read, .write = write},
                 .data = data,
                 .flags = ccr,
                 .indirect = true});
          }
        }
      }
    }
  }
}

void selected_cases() {
  constexpr std::array<unsigned, 4> flags{0, 1, 0xfe, 0xff};
  for (const auto operation : operations) {
    for (const auto ccr : flags) {
      one({.operation = operation,
           .accept = {.pointer = false},
           .data = 0,
           .flags = ccr,
           .indirect = true});
      one({.operation = operation,
           .accept = {},
           .data = 0,
           .flags = ccr,
           .indirect = true,
           .alias = true});
      one({.operation = operation,
           .accept = {.write = false},
           .data = 0,
           .flags = ccr,
           .indirect = true,
           .alias = true});
      one({.operation = operation, .accept = {}, .data = 0, .flags = ccr, .partial = true});
      one({.operation = operation,
           .accept = {.write = false},
           .data = 0,
           .flags = ccr,
           .partial = true});
    }
  }
}

void auto_cases() {
  constexpr std::array<int, 4> changes{-8, -1, 1, 8};
  for (const auto operation : operations) {
    for (unsigned base = 0; base < 3; ++base) {
      for (const auto delta : changes) {
        for (const bool pre : {false, true}) {
          for (const bool write : {false, true}) {
            one({.operation = operation,
                 .accept = {.write = write},
                 .data = 0x80,
                 .flags = byte_mask,
                 .base = base,
                 .delta = delta,
                 .pre = pre});
          }
        }
      }
    }
  }
}
}

void external_cases() {
  phase_matrix();
  selected_cases();
  auto_cases();
}
}
