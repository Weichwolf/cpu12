// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
using Opcode = isa::Opcode;
constexpr Address entry = 0x0200, data = 0x4000, alias = 0x5000, stack = 0x9000;
constexpr Address limit = 0x8000, clobbered = 0x4321, marker = 0x1357;
constexpr unsigned byte_bits = 8, byte_mask = 255, word_mask = 65535, word_values = 65536;
constexpr unsigned sign = 32768, bias = 3, step = 7, sentinel = 0xa5;
constexpr unsigned head_compare = 6, head_branch = 9, tail_add = 11, tail_store = 14;
constexpr unsigned frame_bytes = 9, stack_bytes = 16;

struct Form {
  bool memory = false, y = false, increment = false, clobber = false, byte_store = false;
  bool compare_tail = false;
  bool stack_loaded = false;
  Opcode backedge = Opcode::bne_relative;
};

struct Program {
  std::vector<Byte> code;
  Address clobber_pc = 0, byte_pc = 0, stack_pc = 0, tail_guard_pc = 0, back_pc = 0, exit_pc = 0;
  Opcode guard;
  bool stop = false;
};

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

void append(std::vector<Byte> &code, const Opcode opcode, const Address operand) {
  code.push_back(static_cast<Byte>(opcode));
  code.push_back(static_cast<Byte>(operand >> byte_bits));
  code.push_back(static_cast<Byte>(operand & byte_mask));
}

Program program(const Form form, const Opcode guard, const bool stop = false) {
  Program result{.code = {}, .guard = guard, .stop = stop};
  auto &code = result.code;
  append(code, Opcode::ldd_extended, data);
  append(code, Opcode::addd_immediate, bias);
  append(code,
         form.memory ? Opcode::cpd_extended : Opcode::cpd_immediate,
         form.memory ? data + 6 : limit);
  code.push_back(static_cast<Byte>(guard));
  code.push_back(0); // Resolve the actual exit after the complete tail is assembled.
  append(code, Opcode::addd_immediate, step);
  append(code, Opcode::std_extended, alias);
  if (form.clobber) {
    result.clobber_pc = static_cast<Address>(entry + code.size());
    append(code, Opcode::ldd_immediate, clobbered);
  }
  if (form.byte_store) {
    result.byte_pc = static_cast<Address>(entry + code.size());
    append(code, Opcode::staa_extended, alias + 1);
  }
  if (form.stack_loaded) {
    result.stack_pc = static_cast<Address>(entry + code.size());
    append(code, Opcode::lds_immediate, stack);
  }
  result.tail_guard_pc = static_cast<Address>(entry + code.size());
  constexpr std::array counters{std::array{Opcode::dex, Opcode::inx},
                                std::array{Opcode::dey, Opcode::iny}};
  const auto counter =
      counters.at(static_cast<std::size_t>(form.y)).at(static_cast<std::size_t>(form.increment));
  if (form.compare_tail) {
    append(code,
           form.memory ? Opcode::cpd_extended : Opcode::cpd_immediate,
           form.memory ? data + 6 : limit);
  } else {
    code.push_back(static_cast<Byte>(counter));
  }
  result.back_pc = static_cast<Address>(entry + code.size());
  code.push_back(static_cast<Byte>(form.backedge));
  const auto next = static_cast<unsigned>(code.size() + 1);
  constexpr unsigned byte_values = 256;
  code.push_back(static_cast<Byte>(byte_values - next));
  result.exit_pc = static_cast<Address>(entry + code.size());
  code[head_branch + 1] = static_cast<Byte>(code.size() - tail_add);
  if (stop) { code.insert(code.end(), isa::stop.begin(), isa::stop.end()); }
  code.insert(code.end(), isa::wai.begin(), isa::wai.end());
  return result;
}

struct Snapshot {
  Registers registers;
  std::array<Byte, 8> bytes{};
  std::array<Byte, stack_bytes> stack{};
  ExecutionState execution = ExecutionState::running;
};

void word(Snapshot &state, const unsigned offset, const Address value) {
  state.bytes.at(offset) = static_cast<Byte>(value >> byte_bits);
  state.bytes.at(offset + 1) = static_cast<Byte>(value & byte_mask);
}

Address word(const Snapshot &state, const unsigned offset) {
  return static_cast<Address>((unsigned{state.bytes.at(offset)} << byte_bits) |
                              state.bytes.at(offset + 1));
}

int signed_word(const Address value) {
  return value < sign ? int{value} : int{value} - int{word_values};
}

Address arithmetic(Registers &state, const Address value, const bool subtract = false) {
  const auto wide = subtract ? unsigned{state.d} + word_values - value : unsigned{state.d} + value;
  const auto result = static_cast<Address>(wide & word_mask);
  const auto signed_result =
      signed_word(state.d) + (subtract ? -signed_word(value) : signed_word(value));
  const bool overflow = signed_result < -int{sign} || signed_result >= int{sign};
  const bool carry = subtract ? state.d < value : wide >= word_values;
  constexpr unsigned retained = 0xf0;
  state.ccr =
      static_cast<Byte>((state.ccr & retained) | (result == 0 ? 4U : 0U) |
                        (result >= sign ? 8U : 0U) | (overflow ? 2U : 0U) | (carry ? 1U : 0U));
  return result;
}

void transfer(Registers &state, const Address value, const bool wide = true) {
  constexpr unsigned retained = 0xf1;
  state.ccr = static_cast<Byte>((state.ccr & retained) | (value == 0 ? 4U : 0U) |
                                (value >= (wide ? sign : 128U) ? 8U : 0U));
}

bool taken(const Opcode guard, const Byte flags) {
  if (guard == Opcode::bcc_relative) { return (flags & 1U) == 0; }
  if (guard == Opcode::bcs_relative) { return (flags & 1U) != 0; }
  if (guard == Opcode::bne_relative) { return (flags & 4U) == 0; }
  return (flags & 4U) != 0;
}

void wait_frame(Snapshot &snapshot, const Byte length = 1) {
  auto &state = snapshot.registers;
  state.pc = static_cast<Address>(state.pc + length);
  state.sp -= frame_bytes;
  const std::array<Byte, frame_bytes> frame{state.ccr,
                                            static_cast<Byte>(state.d & byte_mask),
                                            static_cast<Byte>(state.d >> byte_bits),
                                            static_cast<Byte>(state.x >> byte_bits),
                                            static_cast<Byte>(state.x & byte_mask),
                                            static_cast<Byte>(state.y >> byte_bits),
                                            static_cast<Byte>(state.y & byte_mask),
                                            static_cast<Byte>(state.pc >> byte_bits),
                                            static_cast<Byte>(state.pc & byte_mask)};
  std::ranges::copy(frame, snapshot.stack.begin() + (stack_bytes - frame_bytes));
  snapshot.execution = ExecutionState::waiting;
}

void tail_instruction(Snapshot &snapshot, const Form form, const Program &code) {
  auto &state = snapshot.registers;
  const auto pc = state.pc;
  if (pc == entry + tail_add) {
    state.d = arithmetic(state, step);
    state.pc += 3;
  } else if (pc == entry + tail_store) {
    word(snapshot, 0, state.d);
    transfer(state, state.d);
    state.pc += 3;
  } else if (pc == code.clobber_pc) {
    state.d = clobbered;
    transfer(state, state.d);
    state.pc += 3;
  } else if (pc == code.byte_pc) {
    const auto value = static_cast<Address>(state.d >> byte_bits);
    snapshot.bytes[1] = static_cast<Byte>(value);
    transfer(state, value, false);
    state.pc += 3;
  } else if (pc == code.stack_pc) {
    state.sp = stack;
    transfer(state, stack);
    state.pc += 3;
  } else if (pc == code.tail_guard_pc && form.compare_tail) {
    static_cast<void>(arithmetic(state, form.memory ? word(snapshot, 6) : limit, true));
    state.pc += 3;
  } else if (pc == code.tail_guard_pc) {
    auto &counter = form.y ? state.y : state.x;
    counter =
        static_cast<Address>((unsigned{counter} + (form.increment ? 1U : word_mask)) & word_mask);
    state.ccr = static_cast<Byte>((state.ccr & ~4U) | (counter == 0 ? 4U : 0U));
    ++state.pc;
  } else if (pc == code.back_pc) {
    state.pc = form.backedge == Opcode::bra_relative || taken(form.backedge, state.ccr)
                   ? entry
                   : code.exit_pc;
  } else {
    throw std::runtime_error("Unexpected independent tail PC");
  }
}

void instruction(Snapshot &snapshot, const Form form, const Program &code) {
  auto &state = snapshot.registers;
  const auto pc = state.pc;
  if (pc == entry) {
    state.d = word(snapshot, 0);
    transfer(state, state.d);
    state.pc += 3;
  } else if (pc == entry + 3) {
    state.d = arithmetic(state, bias);
    state.pc += 3;
  } else if (pc == entry + head_compare) {
    static_cast<void>(arithmetic(state, form.memory ? word(snapshot, 6) : limit, true));
    state.pc += 3;
  } else if (pc == entry + head_branch) {
    state.pc = taken(code.guard, state.ccr) ? code.exit_pc : entry + tail_add;
  } else if (pc == code.exit_pc) {
    if (code.stop && (state.ccr & 0x80U) != 0) {
      state.pc += static_cast<Address>(isa::stop.size());
    } else {
      wait_frame(snapshot, static_cast<Byte>(code.stop ? isa::stop.size() : isa::wai.size()));
      if (code.stop) { snapshot.execution = ExecutionState::stopped; }
    }
  } else if (code.stop && pc == code.exit_pc + isa::stop.size()) {
    wait_frame(snapshot);
  } else {
    tail_instruction(snapshot, form, code);
  }
}

RunResult advance(Snapshot &snapshot, const Form form, const Program &code, const unsigned budget) {
  RunResult result;
  while (result.retired < budget && snapshot.execution == ExecutionState::running) {
    result.pc = snapshot.registers.pc;
    result.opcode = code.code[result.pc - entry];
    instruction(snapshot, form, code);
    ++result.retired;
  }
  result.state = snapshot.execution;
  return result;
}

[[gnu::noinline]] void clobber_callers() noexcept {
  alignas(16) std::array<std::uint64_t, 2> aligned{};
  asm volatile("pxor %%xmm0, %%xmm0\n\t"
               "movaps %%xmm0, (%0)\n\t"
               "pcmpeqd %%xmm1, %%xmm1\n\tpcmpeqd %%xmm2, %%xmm2\n\t"
               "pcmpeqd %%xmm3, %%xmm3\n\tpcmpeqd %%xmm4, %%xmm4\n\t"
               "pcmpeqd %%xmm5, %%xmm5\n\tpcmpeqd %%xmm6, %%xmm6\n\t"
               "pcmpeqd %%xmm7, %%xmm7\n\tpcmpeqd %%xmm8, %%xmm8\n\t"
               "pcmpeqd %%xmm9, %%xmm9\n\tpcmpeqd %%xmm10, %%xmm10\n\t"
               "pcmpeqd %%xmm11, %%xmm11\n\tpcmpeqd %%xmm12, %%xmm12\n\t"
               "pcmpeqd %%xmm13, %%xmm13\n\tpcmpeqd %%xmm14, %%xmm14\n\t"
               "pcmpeqd %%xmm15, %%xmm15\n\t"
               "xor %%eax, %%eax\n\txor %%ecx, %%ecx\n\txor %%edx, %%edx\n\t"
               "xor %%edi, %%edi\n\txor %%esi, %%esi\n\txor %%r8d, %%r8d\n\t"
               "xor %%r9d, %%r9d\n\txor %%r10d, %%r10d\n\txor %%r11d, %%r11d"
               :
               : "r"(aligned.data())
               : "rax",
                 "rcx",
                 "rdx",
                 "rdi",
                 "rsi",
                 "r8",
                 "r9",
                 "r10",
                 "r11",
                 "xmm0",
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
                 "memory",
                 "cc");
}

struct Access {
  Address address = 0, value = 0;
  Byte size = 0;
  bool operator==(const Access &) const = default;
};

struct StackPort {
  Core *core = nullptr;
  std::array<Byte, stack_bytes> *image = nullptr;
  Registers before{};
  std::array<Access, 5> expected{};
  unsigned calls = 0, refuse = 0;
  bool observed = true;

  void prepare(const unsigned failure, const Registers registers, const Byte length) noexcept {
    before = registers;
    const auto pc = static_cast<Address>(registers.pc + length);
    const auto reversed_d =
        static_cast<Address>((unsigned{registers.d} << byte_bits) | (registers.d >> byte_bits));
    expected = {
        Access{.address = static_cast<Address>(registers.sp - 2), .value = pc, .size = 2},
        Access{.address = static_cast<Address>(registers.sp - 4), .value = registers.y, .size = 2},
        Access{.address = static_cast<Address>(registers.sp - 6), .value = registers.x, .size = 2},
        Access{.address = static_cast<Address>(registers.sp - 8), .value = reversed_d, .size = 2},
        Access{
            .address = static_cast<Address>(registers.sp - 9), .value = registers.ccr, .size = 1}};
    calls = 0;
    refuse = failure;
    observed = true;
  }

  bool receive(const Access access) noexcept {
    if (calls >= expected.size() || access != expected[calls]) {
      observed = false;
      return false;
    }
    observed &= core->registers() == before && core->state().execution == ExecutionState::running;
    ++calls;
    const bool accepted = calls != refuse;
    if (accepted) {
      const auto offset = std::size_t{access.address} - (stack - stack_bytes);
      (*image)[offset] =
          static_cast<Byte>(access.size == 2 ? access.value >> byte_bits : access.value);
      if (access.size == 2) { (*image)[offset + 1] = static_cast<Byte>(access.value & byte_mask); }
    }
    clobber_callers();
    return accepted;
  }

  static bool write(void *object, const ByteWrite access) noexcept {
    return static_cast<StackPort *>(object)->receive(
        {.address = access.address, .value = access.value, .size = 1});
  }

  static bool write_word(void *object, const WordWrite access) noexcept {
    return static_cast<StackPort *>(object)->receive(
        {.address = access.address, .value = access.value, .size = 2});
  }
};

struct Device {
  std::array<Byte, 8> bytes{};
  std::array<Byte, stack_bytes> stack_data{};
  std::array<Ram, 4> regions;
  StackPort port;
  Io io;
  Core core;

  explicit Device(Program &code, const bool external_stack = false)
      : regions{Ram{.first = entry, .bytes = code.code, .writable = false},
                Ram{.first = data, .bytes = bytes},
                Ram{.first = alias, .bytes = bytes},
                Ram{.first = stack - stack_bytes,
                    .bytes = external_stack ? std::span<Byte>{} : std::span(stack_data)}},
        port{.image = &stack_data},
        io{.owner = &port, .write = &StackPort::write, .write_word = &StackPort::write_word},
        core(Memory::mapped(regions), external_stack ? &io : nullptr) {
    port.core = &core;
  }

  void reset(const Snapshot &snapshot) {
    bytes = snapshot.bytes;
    stack_data = snapshot.stack;
    core.set_registers(snapshot.registers);
  }

  void check(const Snapshot &expected, const RunResult result) const {
    const State state{
        .registers = expected.registers, .failure = {}, .execution = expected.execution};
    if (core.state() != state || bytes != expected.bytes || stack_data != expected.stack) {
      std::cerr << "pc=" << core.registers().pc << "/" << state.registers.pc
                << " d=" << core.registers().d << "/" << state.registers.d
                << " ccr=" << unsigned{core.registers().ccr} << "/" << unsigned{state.registers.ccr}
                << " retired=" << result.retired << '\n';
    }
    require(core.state() == state && bytes == expected.bytes && stack_data == expected.stack,
            "Connected region state, aliases, load forwarding or WAI frame differs");
  }
};

struct Point {
  Byte flags;
  Address value, count;
};

Snapshot initial(const Form form, const Point point) {
  Snapshot result{
      .registers = {
          .pc = entry, .d = marker, .x = marker, .y = marker, .sp = stack, .ccr = point.flags}};
  (form.y ? result.registers.y : result.registers.x) = point.count;
  result.bytes.fill(sentinel);
  result.stack.fill(sentinel);
  word(result, 0, point.value);
  word(result, 6, limit);
  return result;
}

unsigned resumed_cases(Device &device, const Form form, const Program &code) {
  constexpr unsigned total = 97;
  constexpr Byte flags = 0x7f;
  constexpr std::array<unsigned, 5> prefixes{1, 3, 5, 7, 9};
  const auto before = initial(
      form,
      {.flags = flags, .value = 0, .count = static_cast<Address>(form.increment ? word_mask : 1)});
  unsigned checked = 0;
  for (const auto prefix : prefixes) {
    auto expected = before;
    device.reset(before);
    auto wanted = advance(expected, form, code, prefix);
    auto actual = device.core.run(prefix);
    device.check(expected, actual);
    require(actual == wanted, "A region prefix must stop at its exact instruction boundary");
    if (expected.execution == ExecutionState::running) {
      wanted = advance(expected, form, code, total - prefix);
      actual = device.core.run(total - prefix);
      device.check(expected, actual);
      require(actual == wanted, "Resuming within a region must preserve prior progress and memory");
    }
    ++checked;
  }
  return checked;
}

void maximum_grant(Device &device, const Form form, const Program &code, const Snapshot &before) {
  auto expected = before;
  constexpr unsigned oracle_limit = 97;
  const auto wanted = advance(expected, form, code, oracle_limit);
  require(expected.execution == ExecutionState::waiting,
          "The extreme-grant fixture must have an independently bounded exit");
  device.reset(before);
  const auto actual = device.core.run(std::numeric_limits<std::uint32_t>::max());
  device.check(expected, actual);
  require(actual == wanted, "An extreme grant must retain exact original progress and state");
}

unsigned extreme_grants(Device &device, const Form form, const Program &code) {
  constexpr Address equal_input = 0x7ffd;
  constexpr Byte flags = 0xff;
  const bool low_exit = code.guard == Opcode::bcs_relative || code.guard == Opcode::bne_relative;
  const auto count = static_cast<Address>(form.increment ? word_mask : 1);
  maximum_grant(device,
                form,
                code,
                initial(form,
                        {.flags = flags,
                         .value = static_cast<Address>(low_exit ? 0 : equal_input),
                         .count = count}));
  if (form.compare_tail || form.backedge != Opcode::bne_relative) { return 1; }
  maximum_grant(device,
                form,
                code,
                initial(form,
                        {.flags = flags,
                         .value = static_cast<Address>(low_exit ? equal_input : 0),
                         .count = count}));
  return 2;
}

unsigned cases(const Form form, const Opcode guard) {
  auto code = program(form, guard);
  Device device(code);
  unsigned checked = 0;
  std::uint64_t compiled = 0;
  constexpr std::array<Address, 5> values{0, 1, 0x7ffd, 0x8000, 0xffff};
  constexpr std::array<Address, 5> counts{0, 1, 2, 7, 3};
  constexpr std::array<unsigned, 15> grants{1, 2, 3, 4, 5, 7, 8, 9, 10, 15, 16, 17, 23, 40, 97};
  for (unsigned flags = 0; flags <= byte_mask; ++flags) {
    for (unsigned point = 0; point < values.size(); ++point) {
      const auto count = form.increment
                             ? static_cast<Address>((word_values - counts[point]) & word_mask)
                             : counts[point];
      const auto before = initial(
          form, {.flags = static_cast<Byte>(flags), .value = values[point], .count = count});
      for (const auto grant : grants) {
        auto expected = before;
        const auto wanted = advance(expected, form, code, grant);
        device.reset(before);
        const auto result = device.core.run(grant);
        device.check(expected, result);
        require(result == wanted,
                "Region exit must report the actual original instruction and progress");
        ++checked;
      }
    }
    const auto current = device.core.statistics().compiled_entries;
    if (flags == 0) { compiled = current; }
    require(current == compiled, "Warm regions must retain their code across flags and deadlines");
  }
  return checked + resumed_cases(device, form, code) + extreme_grants(device, form, code);
}

struct SleepPoint {
  Snapshot before;
  unsigned retired = 0;
  Byte length = 1, opcode = 0;
};

enum class Grant : Byte { all, exact };

SleepPoint before_sleep(const Form form, const Program &code, const Snapshot &initial_state) {
  SleepPoint result{.before = initial_state};
  while (result.before.registers.pc != code.exit_pc) {
    instruction(result.before, form, code);
    ++result.retired;
  }
  if (code.stop && (result.before.registers.ccr & 0x80U) != 0) {
    instruction(result.before, form, code);
    ++result.retired;
  }
  result.length = static_cast<Byte>(
      code.stop && result.before.registers.pc == code.exit_pc ? isa::stop.size() : isa::wai.size());
  result.opcode = code.code[result.before.registers.pc - entry];
  return result;
}

void expected_writes(Snapshot &snapshot, const StackPort &port, const unsigned accepted) {
  for (unsigned index = 0; index < accepted; ++index) {
    const auto access = port.expected.at(index);
    const auto offset = std::size_t{access.address} - (stack - stack_bytes);
    snapshot.stack.at(offset) =
        static_cast<Byte>(access.size == 2 ? access.value >> byte_bits : access.value);
    if (access.size == 2) {
      snapshot.stack.at(offset + 1) = static_cast<Byte>(access.value & byte_mask);
    }
  }
}

void owner_case(Device &device,
                const Form form,
                const Program &code,
                const Point point,
                const unsigned refusal,
                const Grant grant) {
  const auto before = initial(form, point);
  const auto sleep = before_sleep(form, code, before);
  device.reset(before);
  device.port.prepare(refusal, sleep.before.registers, sleep.length);
  auto expected = sleep.before;
  RunResult wanted{
      .retired = sleep.retired, .pc = sleep.before.registers.pc, .opcode = sleep.opcode};
  State state{};
  if (refusal == 0) {
    instruction(expected, form, code);
    ++wanted.retired;
    wanted.state = expected.execution;
  } else {
    expected_writes(expected, device.port, refusal - 1U);
    const auto refused = device.port.expected.at(refusal - 1U);
    wanted.fault = Fault::memory_write;
    wanted.address = refused.address;
    wanted.value = refused.value;
    wanted.access_size = refused.size;
    state.failure = wanted;
    state.failure.retired = 0;
  }
  state.registers = expected.registers;
  state.execution = expected.execution;
  const auto budget =
      grant == Grant::all ? std::numeric_limits<std::uint32_t>::max() : sleep.retired + 1U;
  const auto actual = device.core.run(budget);
  require(actual == wanted && device.core.state() == state && device.bytes == expected.bytes &&
              device.stack_data == expected.stack && device.port.observed &&
              device.port.calls == (refusal == 0 ? 5U : refusal),
          "Connected STOP/WAI lost original owner order, context, state, progress or stack bytes");
  if (refusal != 0) {
    const auto calls = device.port.calls;
    require(device.core.run() == state.failure && device.port.calls == calls &&
                device.core.state() == state && device.stack_data == expected.stack,
            "A refused connected frame must latch without replaying accepted writes");
  }
}

unsigned owner_cases(const Form form, const bool stop) {
  auto code = program(form, Opcode::bcc_relative, stop);
  Device device(code, true);
  unsigned checked = 0;
  constexpr std::array<Address, 2> values{0, limit - bias};
  for (unsigned flags = 0; flags <= byte_mask; ++flags) {
    for (const auto value : values) {
      for (unsigned refusal = 0; refusal <= 5; ++refusal) {
        for (const auto grant : {Grant::all, Grant::exact}) {
          owner_case(device,
                     form,
                     code,
                     {.flags = static_cast<Byte>(flags), .value = value, .count = 1},
                     refusal,
                     grant);
          ++checked;
        }
      }
    }
  }
  return checked;
}

void readonly_d_case(Device &device, const Program &code, const Point point) {
  auto before = initial({}, {.flags = point.flags, .value = 0, .count = point.count});
  before.registers.d = point.value;
  auto expected = before;
  const bool early = point.value < 256;
  constexpr unsigned retained = 0xf0;
  expected.registers.x = early ? point.count : 0;
  expected.registers.pc = code.exit_pc;
  expected.registers.ccr = static_cast<Byte>((point.flags & retained) | 4U |
                                             (!early && (point.value & 0x80U) != 0 ? 8U : 0U));
  wait_frame(expected);
  const RunResult wanted{.retired = early ? 3U : (5U * point.count) + 1U,
                         .state = ExecutionState::waiting,
                         .pc = code.exit_pc,
                         .opcode = isa::wai[0]};
  for (const auto grant : {Grant::all, Grant::exact}) {
    device.reset(before);
    const auto result = device.core.run(grant == Grant::all ? word_mask : wanted.retired);
    device.check(expected, result);
    require(result == wanted, "Byte-only comparisons must preserve the original D frame");
  }
}

unsigned readonly_d_cases() {
  constexpr std::array<Byte, 1> zero{0};
  constexpr std::array<Byte, 1> exit{5};
  constexpr std::array<Byte, 1> back{256 - 9};
  constexpr auto bytes = isa::program(isa::opcode<Opcode::cmpa_immediate>(),
                                      zero,
                                      isa::opcode<Opcode::beq_relative>(),
                                      exit,
                                      isa::opcode<Opcode::cmpb_immediate>(),
                                      zero,
                                      isa::dex,
                                      isa::opcode<Opcode::bne_relative>(),
                                      back,
                                      isa::wai);
  Program code{.code = {bytes.begin(), bytes.end()},
               .exit_pc = entry + bytes.size() - isa::wai.size(),
               .guard = Opcode::beq_relative};
  Device device(code);
  unsigned checked = 0;
  constexpr std::array<Address, 8> values{
      0x1357, 0x5700, 0x0080, 0x8000, 0xff00, 0x00ff, 0x807f, 0x7f80};
  for (unsigned flags = 0; flags <= byte_mask; ++flags) {
    for (const auto value : values) {
      for (const Address count : {Address{1}, Address{2}}) {
        readonly_d_case(
            device, code, {.flags = static_cast<Byte>(flags), .value = value, .count = count});
        checked += 2;
      }
    }
  }
  return checked;
}
}

int main() {
  try {
    const auto readonly = readonly_d_cases();
    constexpr std::array forms{
        Form{},
        Form{.memory = true, .y = true},
        Form{.increment = true},
        Form{.memory = true, .clobber = true},
        Form{.y = true, .increment = true, .byte_store = true},
        Form{.memory = true, .y = true, .clobber = true, .byte_store = true},
        Form{.memory = true, .backedge = Opcode::bra_relative},
        Form{.compare_tail = true, .backedge = Opcode::bcc_relative},
        Form{.memory = true, .compare_tail = true, .backedge = Opcode::bcs_relative},
        Form{.stack_loaded = true},
        Form{.memory = true, .y = true, .stack_loaded = true}};
    constexpr std::array guards{
        Opcode::bcc_relative, Opcode::bcs_relative, Opcode::bne_relative, Opcode::beq_relative};
    unsigned checked = 0;
    for (const auto form : forms) {
      for (const auto guard : guards) { checked += cases(form, guard); }
    }
    unsigned owners = 0;
    for (const auto form : {Form{},
                            Form{.y = true},
                            Form{.stack_loaded = true},
                            Form{.y = true, .stack_loaded = true}}) {
      for (const auto stop : {false, true}) { owners += owner_cases(form, stop); }
    }
    std::cout << checked << " connected region cases and " << owners
              << " real STOP/WAI owner/frame/fault cases and " << readonly
              << " read-only D frame cases passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
