// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "stack_model.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace cpu12::test::stack {
namespace {
struct Event {
  std::uint16_t address;
  unsigned width;
  bool write;
};

struct Port {
  Core *core = nullptr;
  Registers before{};
  std::array<Byte, memory_bytes> ram{};
  std::array<Event, 4> events{};
  unsigned used = 0;
  bool accept_read = false, accept_write = false, observed = true;

  void observe(const Event event) noexcept {
    observed = observed && core->registers() == before && used < events.size();
    if (used < events.size()) { events[used] = event; }
    ++used;
  }

  static bool read_byte(void *owner, const std::uint16_t address, Byte &data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.observe({.address = address, .width = 1, .write = false});
    data = port.ram[address];
    return port.accept_read;
  }

  static bool write_byte(void *owner, const ByteWrite data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.observe({.address = data.address, .width = 1, .write = true});
    port.ram[data.address] = data.value;
    return port.accept_write;
  }

  static bool read_word(void *owner, const std::uint16_t address, std::uint16_t &data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.observe({.address = address, .width = 2, .write = false});
    data = word(port.ram, address);
    return port.accept_read;
  }

  static bool write_word(void *owner, const WordWrite data) noexcept {
    auto &port = *static_cast<Port *>(owner);
    port.observe({.address = data.address, .width = 2, .write = true});
    port.ram[data.address] = static_cast<Byte>(data.value >> byte_bits);
    if (port.accept_write) { port.ram[next(data.address)] = static_cast<Byte>(data.value); }
#if defined(__x86_64__)
    // An actual external callback may freely clobber this SysV caller-saved register.
    asm volatile("movabs $0xabcdef1234567890, %%r9" : : : "r9");
#endif
    return port.accept_write;
  }

  [[nodiscard]] Io io() noexcept {
    return {.owner = this,
            .read = read_byte,
            .write = write_byte,
            .read_word = read_word,
            .write_word = write_word};
  }
};

void refusal(Core &core,
             const std::span<const Byte> code,
             const Registers before,
             const RunResult fault) {
  const auto result = core.run_instruction(code);
  require(
      result.retired == 0 && result.fault == fault.fault && result.pc == before.pc &&
          result.opcode == code[0] && result.address == fault.address &&
          result.value == fault.value && result.access_size == fault.access_size &&
          core.registers() == before,
      "Refused stack/call access must retain exact actual data, width, address and original CPU");
  core.clear_code_cache();
  const auto count = core.statistics();
  require(core.run_instruction(code).retired == 0 &&
              core.statistics().read_callbacks == count.read_callbacks &&
              core.statistics().write_callbacks == count.write_callbacks,
          "A latched stack/call fault cannot replay real external effects");
}

void inherent(const StackOp operation) {
  for (const bool accept : {false, true}) {
    Port port{.before = seeds[0], .accept_read = accept, .accept_write = accept};
    auto io = port.io();
    Core core(Memory::external(), &io);
    port.core = &core;
    core.set_registers(port.before);
    constexpr std::uint16_t operand = 0x1234;
    const auto data =
        static_cast<std::uint16_t>(operation.width == 2 ? operand : operand & byte_mask);
    const auto wanted = expected(operation, port.before, data);
    const auto address = operation.push ? wanted.sp : port.before.sp;
    if (operation.width == 2) {
      word(port.ram, address, data);
    } else {
      port.ram[address] = static_cast<Byte>(data);
    }
    if (operation.push) {
      constexpr Byte previous = 0xa5;
      port.ram[address] = previous;
    }
    const std::array code{operation.opcode};
    if (accept) {
      retired(core, code, port.before, wanted);
    } else {
      refusal(core,
              code,
              port.before,
              {.fault = operation.push ? Fault::memory_write : Fault::memory_read,
               .address = address,
               .value = operation.push ? value(port.before, operation.target) : data,
               .access_size = static_cast<Byte>(operation.width)});
    }
    require(port.observed && port.used == 1 && port.events[0].address == address &&
                port.events[0].width == operation.width && port.events[0].write == operation.push,
            "Each inherent stack operation makes exactly its declared byte/word bus request");
    if (operation.push) {
      const auto source = value(port.before, operation.target);
      require(port.ram[address] == (operation.width == 2 ? source >> byte_bits : source),
              "Rejected writes retain actual byte effects instead of pretending rollback");
    }
  }
}

struct Acceptance {
  bool read, write;
};

void indirect_case(const Acceptance acceptance) {
  constexpr std::uint16_t pointer = 0x2000;
  constexpr std::uint16_t target = 0x5432;
  constexpr std::array<Byte, 4> code{0x15, 0xe3, 0, 0};
  const auto read = acceptance.read;
  const auto write = acceptance.write;
  Port port{.before = seeds[0], .accept_read = read, .accept_write = write};
  port.before.x = pointer;
  word(port.ram, pointer, target);
  auto io = port.io();
  Core core(Memory::external(), &io);
  port.core = &core;
  core.set_registers(port.before);
  const auto stack = static_cast<std::uint16_t>(static_cast<int>(port.before.sp) - 2);
  const auto return_pc = static_cast<std::uint16_t>(port.before.pc + code.size());
  if (read && write) {
    auto wanted = port.before;
    wanted.pc = target;
    wanted.sp = stack;
    call(core, port.ram, code, port.before, wanted);
  } else {
    refusal(core,
            code,
            port.before,
            {.fault = read ? Fault::memory_write : Fault::memory_read,
             .address = read ? stack : pointer,
             .value = read ? return_pc : target,
             .access_size = 2});
  }
  require(port.observed && port.used == (read ? 2U : 1U) && !port.events[0].write &&
              port.events[0].address == pointer && port.events[0].width == 2,
          "Indirect call must read its actual pointer before any return-stack write");
  if (read) {
    require(port.events[1].write && port.events[1].address == stack && port.events[1].width == 2 &&
                port.ram[stack] == return_pc >> byte_bits,
            "Indirect target survives real caller-saved clobbers and refused partial writes");
  }
}

void indirect_call() {
  for (const bool read : {false, true}) {
    for (const bool write : {false, true}) { indirect_case({.read = read, .write = write}); }
  }
}

void ordinary_calls() {
  struct Call {
    std::array<Byte, 3> code;
    unsigned length;
    std::uint16_t target;
  };

  constexpr std::array<Call, 4> calls{{{.code = {0x16, 0x12, 0x34}, .length = 3, .target = 0x1234},
                                       {.code = {0x17, 0x56, 0}, .length = 2, .target = 0x56},
                                       {.code = {7, 0xfe, 0}, .length = 2, .target = 0xfffe},
                                       {.code = {0x15, 0, 0}, .length = 2, .target = 0x1234}}};
  for (const auto point : calls) {
    for (const bool accept : {false, true}) {
      Port port{.before = seeds[0], .accept_write = accept};
      auto io = port.io();
      Core core(Memory::external(), &io);
      port.core = &core;
      core.set_registers(port.before);
      const auto code = std::span(point.code).first(point.length);
      auto wanted = port.before;
      wanted.pc = point.target;
      wanted.sp = static_cast<std::uint16_t>(static_cast<int>(wanted.sp) - 2);
      if (accept) {
        call(core, port.ram, code, port.before, wanted);
      } else {
        refusal(core,
                code,
                port.before,
                {.fault = Fault::memory_write,
                 .address = wanted.sp,
                 .value = static_cast<std::uint16_t>(port.before.pc + code.size()),
                 .access_size = 2});
      }
      require(port.observed && port.used == 1 && port.events[0].write &&
                  port.events[0].address == wanted.sp && port.events[0].width == 2 &&
                  core.statistics().read_callbacks == 0,
              "Ordinary calls perform exactly the return-stack write and no target data read");
    }
  }
}

void window_seams() {
  constexpr std::uint16_t first = 0xffff;
  constexpr StackOp push_x{.opcode = 0x34, .target = Target::x, .width = 2, .push = true};
  for (const bool accept : {false, true}) {
    Port port{.before = seeds[0], .accept_write = accept};
    auto io = port.io();
    Core core(Memory::window({.first = first, .bytes = std::span(port.ram).subspan(first, 1)}),
              &io);
    port.core = &core;
    core.set_registers(port.before);
    constexpr Byte untouched = 0xa5;
    port.ram[0] = untouched;
    const auto wanted = expected(push_x, port.before, 0);
    const std::array code{push_x.opcode};
    if (accept) {
      retired(core, code, port.before, wanted);
      require(word(port.ram, first) == port.before.x,
              "Accepted seam write stores the actual source word");
    } else {
      refusal(core,
              code,
              port.before,
              {.fault = Fault::memory_write,
               .address = first,
               .value = port.before.x,
               .access_size = 2});
    }
    require(port.used == 1 && port.observed && port.events[0].address == first &&
                port.events[0].width == 2 && port.events[0].write &&
                (accept || port.ram[0] == untouched),
            "A stack word crossing the RAM seam delegates the full request and preserves partial "
            "effects");
  }
}
}

void external_bus() {
  for (const auto operation : operations) { inherent(operation); }
  indirect_call();
  ordinary_calls();
  window_seams();
}
}
