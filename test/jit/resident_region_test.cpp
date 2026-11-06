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
  Opcode backedge = Opcode::bne_relative;
};

struct Program {
  std::vector<Byte> code;
  Address clobber_pc = 0, byte_pc = 0, tail_guard_pc = 0, back_pc = 0, exit_pc = 0;
  Opcode guard;
};

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

void append(std::vector<Byte> &code, const Opcode opcode, const Address operand) {
  code.push_back(static_cast<Byte>(opcode));
  code.push_back(static_cast<Byte>(operand >> byte_bits));
  code.push_back(static_cast<Byte>(operand & byte_mask));
}

Program program(const Form form, const Opcode guard) {
  Program result{.code = {}, .guard = guard};
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
  code.push_back(static_cast<Byte>(Opcode::wai));
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

void wait_frame(Snapshot &snapshot) {
  auto &state = snapshot.registers;
  ++state.pc;
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

struct Device {
  std::array<Byte, 8> bytes{};
  std::array<Byte, stack_bytes> stack_data{};
  std::array<Ram, 4> regions;
  Core core;

  explicit Device(Program &code)
      : regions{Ram{.first = entry, .bytes = code.code, .writable = false},
                Ram{.first = data, .bytes = bytes},
                Ram{.first = alias, .bytes = bytes},
                Ram{.first = stack - stack_bytes, .bytes = stack_data}},
        core(Memory::mapped(regions)) {}

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
}

int main() {
  try {
    constexpr std::array forms{
        Form{},
        Form{.memory = true, .y = true},
        Form{.increment = true},
        Form{.memory = true, .clobber = true},
        Form{.y = true, .increment = true, .byte_store = true},
        Form{.memory = true, .y = true, .clobber = true, .byte_store = true},
        Form{.memory = true, .backedge = Opcode::bra_relative},
        Form{.compare_tail = true, .backedge = Opcode::bcc_relative},
        Form{.memory = true, .compare_tail = true, .backedge = Opcode::bcs_relative}};
    constexpr std::array guards{
        Opcode::bcc_relative, Opcode::bcs_relative, Opcode::bne_relative, Opcode::beq_relative};
    unsigned checked = 0;
    for (const auto form : forms) {
      for (const auto guard : guards) { checked += cases(form, guard); }
    }
    std::cout << checked
              << " connected native region, alias, flag, deadline and WAI cases passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
