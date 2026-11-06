// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"
#include "observed_core.hpp"

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
constexpr Address entry = 0x0200, data_address = 0x4000, data_alias = 0x5000;
constexpr Address wait_pc = entry + 20, marker = 0x1234, limit = 0x8000;
constexpr Address counter_pc = entry + 17, backedge_pc = entry + 18;
constexpr unsigned byte_bits = 8, byte_mask = 255, word_mask = 65535, word_values = 65536;
constexpr unsigned frame_bytes = 9, guards = 2, frame_storage = frame_bytes + (2 * guards);
constexpr unsigned phases = 5, sentinel = 0xa5, prefix_limit = 64;
constexpr unsigned bias = 3, step = 7, sign = 32768;
using Data = std::array<Byte, 6>;
using Frame = std::array<Byte, frame_storage>;

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

Address wrap(const unsigned value) {
  return static_cast<Address>(value & word_mask);
}

void word(Data &data, const Address value) {
  data[guards] = static_cast<Byte>(value >> byte_bits);
  data[guards + 1] = static_cast<Byte>(value & byte_mask);
}

Address word(const Data &data) {
  return static_cast<Address>((unsigned{data[guards]} << byte_bits) | data[guards + 1]);
}

void append(std::vector<Byte> &code, const Opcode opcode, const Address operand) {
  code.push_back(static_cast<Byte>(opcode));
  code.push_back(static_cast<Byte>(operand >> byte_bits));
  code.push_back(static_cast<Byte>(operand & byte_mask));
}

struct Mode {
  bool dbne = false, simple = false, quiet = false;
};

std::vector<Byte> program(const Mode mode) {
  std::vector<Byte> code;
  append(code, Opcode::ldd_extended, data_address);
  append(code, Opcode::addd_immediate, bias);
  append(code, Opcode::cpd_immediate, limit);
  if (mode.simple) {
    code.push_back(static_cast<Byte>(Opcode::nop));
    code.push_back(static_cast<Byte>(Opcode::nop));
  } else {
    code.push_back(static_cast<Byte>(Opcode::beq_relative));
    code.push_back(9); // From the head fallthrough at +11 to the genuine WAI at +20.
  }
  append(code, Opcode::addd_immediate, step);
  append(code, Opcode::std_extended, data_alias);
  if (mode.dbne) {
    code.push_back(static_cast<Byte>(Opcode::dbne_relative));
    constexpr Byte decrement_y_negative = 0x36;
    code.push_back(decrement_y_negative);
  } else {
    code.push_back(static_cast<Byte>(Opcode::dey));
    code.push_back(static_cast<Byte>(Opcode::bne_relative));
  }
  constexpr Byte back_to_head = 0xec; // Signed displacement -20 from the common tail end.
  code.push_back(back_to_head);
  if (mode.quiet) {
    std::ranges::fill(std::span(code).first(static_cast<std::size_t>(counter_pc - entry)),
                      static_cast<Byte>(Opcode::nop));
  }
  require(code.size() == wait_pc - entry, "Independent branch layout must end at the WAI PC");
  code.push_back(static_cast<Byte>(Opcode::wai));
  return code;
}

struct Transfer {
  Address address = 0, value = 0;
  bool word = true;
  bool operator==(const Transfer &) const = default;
};

std::array<Transfer, phases> frame_writes(const Registers &registers) {
  const auto swapped = static_cast<Address>((unsigned{registers.d} >> byte_bits) |
                                            ((unsigned{registers.d} & byte_mask) << byte_bits));
  return {{{.address = wrap(unsigned{registers.sp} + word_values - 2), .value = wait_pc + 1},
           {.address = wrap(unsigned{registers.sp} + word_values - 4), .value = registers.y},
           {.address = wrap(unsigned{registers.sp} + word_values - 6), .value = registers.x},
           {.address = wrap(unsigned{registers.sp} + word_values - 8), .value = swapped},
           {.address = wrap(unsigned{registers.sp} + word_values - 9),
            .value = registers.ccr,
            .word = false}}};
}

struct RunGrant {
  unsigned budget, failure;
};

struct Model {
  State state{.registers = {}, .failure = {}};
  Data data{};
  Frame frame{};
  unsigned writes = 0;

  Model() {
    data.fill(sentinel);
    frame.fill(sentinel);
  }

  void transfer_flags() {
    auto &r = state.registers;
    constexpr unsigned retained = 0xf1;
    r.ccr = static_cast<Byte>((r.ccr & retained) | (r.d == 0 ? 4U : 0U) | (r.d >= sign ? 8U : 0U));
  }

  void arithmetic(const Address operand, const bool compare = false) {
    auto &r = state.registers;
    const auto signed_word = [](const Address value) {
      return value < sign ? int{value} : int{value} - int{word_values};
    };
    const auto wide = compare ? unsigned{r.d} + word_values - operand : unsigned{r.d} + operand;
    const auto result = wrap(wide);
    const auto signed_result =
        signed_word(r.d) + (compare ? -signed_word(operand) : signed_word(operand));
    const bool overflow = signed_result < -int{sign} || signed_result >= int{sign};
    const bool carry = compare ? r.d < operand : wide >= word_values;
    constexpr unsigned retained = 0xf0;
    r.ccr =
        static_cast<Byte>((r.ccr & retained) | (result == 0 ? 4U : 0U) |
                          (result >= sign ? 8U : 0U) | (overflow ? 2U : 0U) | (carry ? 1U : 0U));
    if (!compare) { r.d = result; }
  }

  [[nodiscard]] bool nop(const Mode mode) const {
    const auto pc = state.registers.pc;
    return (mode.quiet && pc < counter_pc) || (mode.simple && pc >= entry + 9 && pc < entry + 11);
  }

  void instruction(const Mode mode) {
    auto &r = state.registers;
    if (nop(mode)) {
      ++r.pc;
      return;
    }
    if (r.pc == entry) {
      r.d = word(data);
      transfer_flags();
      r.pc += 3;
    } else if (r.pc == entry + 3) {
      arithmetic(bias);
      r.pc += 3;
    } else if (r.pc == entry + 6) {
      arithmetic(limit, true);
      r.pc += 3;
    } else if (r.pc == entry + 9) {
      r.pc = (r.ccr & 4U) != 0 ? wait_pc : entry + 11;
    } else if (r.pc == entry + 11) {
      arithmetic(step);
      r.pc += 3;
    } else if (r.pc == entry + 14) {
      word(data, r.d);
      transfer_flags();
      r.pc += 3;
    } else if (r.pc == counter_pc) {
      r.y = wrap(unsigned{r.y} + word_mask);
      if (mode.dbne) {
        r.pc = r.y != 0 ? entry : wait_pc;
      } else {
        r.ccr = static_cast<Byte>((r.ccr & ~4U) | (r.y == 0 ? 4U : 0U));
        ++r.pc;
      }
    } else if (!mode.dbne && r.pc == backedge_pc) {
      r.pc = (r.ccr & 4U) == 0 ? entry : wait_pc;
    } else {
      throw std::runtime_error("Unexpected independent region instruction PC");
    }
  }

  RunResult wait(const unsigned failure) {
    const auto transfers = frame_writes(state.registers);
    constexpr std::array<unsigned, phases> slots{7, 5, 3, 1, 0};
    for (unsigned phase = 0; phase < transfers.size(); ++phase) {
      const auto access = transfers[phase];
      const auto slot = guards + slots[phase];
      frame[slot] = static_cast<Byte>(access.word ? access.value >> byte_bits : access.value);
      if (access.word) { frame[slot + 1] = static_cast<Byte>(access.value & byte_mask); }
      ++writes;
      if (phase == failure) {
        RunResult fault{.fault = Fault::memory_write,
                        .pc = wait_pc,
                        .opcode = static_cast<Byte>(Opcode::wai),
                        .address = access.address,
                        .value = access.value,
                        .access_size = static_cast<Byte>(access.word ? 2 : 1)};
        state.failure = fault;
        return fault;
      }
    }
    state.registers.sp = wrap(unsigned{state.registers.sp} + word_values - frame_bytes);
    state.registers.pc = wait_pc + 1;
    state.execution = ExecutionState::waiting;
    return {};
  }

  RunResult advance(const Mode mode, const std::span<const Byte> code, const RunGrant grant) {
    RunResult result{.state = state.execution, .pc = state.registers.pc};
    if (state.failure.fault != Fault::none) { return state.failure; }
    for (unsigned retired = 0;
         retired < grant.budget && state.execution == ExecutionState::running;) {
      result.pc = state.registers.pc;
      result.opcode = code[result.pc - entry];
      if (result.pc == wait_pc) {
        const auto fault = wait(grant.failure);
        if (fault.fault != Fault::none) {
          result = fault;
          result.retired = retired;
          return result;
        }
      } else {
        instruction(mode);
      }
      ++retired;
      result.retired = retired;
      result.state = state.execution;
    }
    return result;
  }
};

void clobber(const unsigned salt) noexcept {
  alignas(16) std::array<std::uint64_t, 2> aligned{salt, ~std::uint64_t{salt}};
  asm volatile("movaps (%0),%%xmm0\n"
               "xor %%eax,%%eax\n xor %%ecx,%%ecx\n xor %%edx,%%edx\n"
               "xor %%esi,%%esi\n xor %%edi,%%edi\n xor %%r8d,%%r8d\n"
               "xor %%r9d,%%r9d\n xor %%r10d,%%r10d\n xor %%r11d,%%r11d\n"
               "pxor %%xmm1,%%xmm1\n pxor %%xmm2,%%xmm2\n pxor %%xmm3,%%xmm3\n"
               "pxor %%xmm4,%%xmm4\n pxor %%xmm5,%%xmm5\n pxor %%xmm6,%%xmm6\n"
               "pxor %%xmm7,%%xmm7\n pxor %%xmm8,%%xmm8\n pxor %%xmm9,%%xmm9\n"
               "pxor %%xmm10,%%xmm10\n pxor %%xmm11,%%xmm11\n pxor %%xmm12,%%xmm12\n"
               "pxor %%xmm13,%%xmm13\n pxor %%xmm14,%%xmm14\n pxor %%xmm15,%%xmm15"
               :
               : "r"(aligned.data())
               : "rax",
                 "rcx",
                 "rdx",
                 "rsi",
                 "rdi",
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
                 "cc",
                 "memory");
}

struct Bus {
  test::ObservedCore *core = nullptr;
  Data *data = nullptr;
  State expected{.registers = {}, .failure = {}};
  Data expected_data{};
  Frame frame{};
  unsigned writes = 0, fetches = 0, failure = phases;
  bool intact = true;

  bool record(const Transfer access) noexcept {
    intact &= core->state() == expected && *data == expected_data;
    intact &= core->step().fault == Fault::reentrant && core->run(1).fault == Fault::reentrant;
    const auto wanted = frame_writes(expected.registers);
    const auto phase = writes++;
    if (phase >= phases) { return intact = false; }
    intact &= access == wanted[phase];
    const auto base = wrap(unsigned{expected.registers.sp} + word_values - frame_bytes);
    const auto offset = wrap(unsigned{access.address} + word_values - base);
    if (offset >= frame_bytes || (access.word && offset + 1U >= frame_bytes)) {
      return intact = false;
    }
    frame[guards + offset] =
        static_cast<Byte>(access.word ? access.value >> byte_bits : access.value);
    if (access.word) { frame[guards + offset + 1] = static_cast<Byte>(access.value & byte_mask); }
    const bool accepted = intact && phase != failure;
    clobber(writes);
    return accepted;
  }

  static bool byte(void *owner, const ByteWrite access) noexcept {
    return static_cast<Bus *>(owner)->record(
        {.address = access.address, .value = access.value, .word = false});
  }

  static bool word(void *owner, const WordWrite access) noexcept {
    return static_cast<Bus *>(owner)->record({.address = access.address, .value = access.value});
  }

  static bool fetch(void *owner, Address /*address*/, Byte & /*value*/) noexcept {
    ++static_cast<Bus *>(owner)->fetches;
    return false;
  }
};

struct Point {
  Address value, count;
};

struct Scenario {
  Point point;
  Address sp;
  Byte flags;
  unsigned failure;
};

struct Device {
  Mode mode;
  std::vector<Byte> code;
  Data data{};
  std::array<Ram, 3> regions;
  Bus bus;
  Io io{.owner = &bus, .write = Bus::byte, .write_word = Bus::word, .fetch = Bus::fetch};
  test::ObservedCore core;

  explicit Device(const Mode requested)
      : mode(requested),
        code(program(requested)),
        regions{{{.first = entry, .bytes = code, .writable = false},
                 {.first = data_address,
                  .bytes = std::span(data).subspan(guards, 2),
                  .writable = false},
                 {.first = data_alias, .bytes = std::span(data).subspan(guards, 2)}}},
        core(Memory::mapped(regions), &io) {
    bus.core = &core;
    bus.data = &data;
  }

  Model reset(const Scenario sample) {
    Model model;
    model.state.registers = {.pc = entry,
                             .d = marker,
                             .x = marker,
                             .y = sample.point.count,
                             .sp = sample.sp,
                             .ccr = sample.flags};
    word(model.data, sample.point.value);
    auto prefix = model;
    unsigned retired = 0;
    while (prefix.state.registers.pc != wait_pc && retired++ < prefix_limit) {
      prefix.instruction(mode);
    }
    require(prefix.state.registers.pc == wait_pc, "Independent prefix must terminate before WAI");
    bus.expected = prefix.state;
    bus.expected_data = prefix.data;
    bus.frame.fill(sentinel);
    bus.writes = 0;
    bus.fetches = 0;
    bus.failure = sample.failure;
    bus.intact = true;
    data = model.data;
    core.set_registers(model.state.registers);
    return model;
  }

  void check(const Model &model, const RunResult actual, const RunResult wanted) const {
    if (actual != wanted || core.state() != model.state || !bus.intact) {
      std::cerr << "pc=" << actual.pc << '/' << wanted.pc << " retired=" << actual.retired << '/'
                << wanted.retired << " fault=" << static_cast<unsigned>(actual.fault) << '/'
                << static_cast<unsigned>(wanted.fault) << " writes=" << bus.writes << '/'
                << model.writes << " owner=" << bus.intact << '\n';
    }
    require(actual == wanted, "Region/WAI progress or fault metadata differs");
    require(core.state() == model.state && data == model.data && bus.frame == model.frame,
            "Region/WAI complete state, aliased RAM, partial frame or guards differ");
    require(bus.intact && bus.writes == model.writes && bus.fetches == 0,
            "WAI owner must observe the exact prefix and every original transfer once");
  }
};

unsigned scenario(Device &device, const Scenario sample, const unsigned quantum) {
  auto model = device.reset(sample);
  const auto before = device.core.statistics();
  device.check(model, device.core.run(0), {.pc = entry});
  unsigned retired = 0;
  unsigned boundaries = 0;
  while (model.state.execution == ExecutionState::running &&
         model.state.failure.fault == Fault::none) {
    const auto wanted =
        model.advance(device.mode, device.code, {.budget = quantum, .failure = sample.failure});
    const auto actual = device.core.run(quantum);
    device.check(model, actual, wanted);
    retired += actual.retired;
    ++boundaries;
    const auto stats = device.core.statistics();
    require(stats.native_instructions - before.native_instructions == retired &&
                stats.write_callbacks - before.write_callbacks == model.writes &&
                stats.fetch_callbacks == before.fetch_callbacks &&
                stats.read_callbacks == before.read_callbacks,
            "Native progress and actual callback counters must agree with the independent trace");
  }
  const auto idle = model.state.failure.fault != Fault::none
                        ? model.state.failure
                        : RunResult{.state = ExecutionState::waiting, .pc = wait_pc + 1};
  device.check(model, device.core.run(0), idle);
  device.check(model, device.core.run(std::numeric_limits<std::uint32_t>::max()), idle);
  return boundaries;
}

struct Counts {
  unsigned scenarios = 0, boundaries = 0;
};

Counts cases(const Mode mode) {
  Device device(mode);
  const std::array points{Point{.value = 0, .count = 3},
                          Point{.value = 0x7ffd, .count = 3},
                          Point{.value = 0x7ff3, .count = mode.simple ? Address{2} : Address{0}},
                          Point{.value = 0xfffa, .count = 1}};
  constexpr std::array<Address, 2> stacks{1, 0x9000};
  constexpr std::array<unsigned, 18> quanta{1,
                                            2,
                                            3,
                                            4,
                                            7,
                                            8,
                                            13,
                                            std::numeric_limits<std::uint32_t>::max(),
                                            9,
                                            10,
                                            16,
                                            17,
                                            18,
                                            19,
                                            24,
                                            25,
                                            27,
                                            28};
  // Simple DEY/BNE and DBNE loops retire 9 and 8 instructions per trip.
  // Include exact complete-loop grants and precisely one extra WAI grant.
  constexpr std::array<unsigned, 14> quiet_quanta{
      1, 2, 3, 4, 7, 8, 13, std::numeric_limits<std::uint32_t>::max(), 18, 19, 36, 37, 54, 55};
  auto grants = std::span<const unsigned>(quanta).first(8);
  if (mode.simple) { grants = quanta; }
  if (mode.quiet) { grants = quiet_quanta; }
  static_cast<void>(
      scenario(device, {.point = points[0], .sp = stacks[1], .flags = 0, .failure = phases}, 1));
  const auto compiled = device.core.statistics().compiled_entries;
  const auto original = device.code;
  Counts counts;
  for (unsigned flags = 0; flags <= byte_mask; ++flags) {
    for (const auto point : points) {
      for (const auto sp : stacks) {
        for (unsigned failure = 0; failure <= phases; ++failure) {
          for (const auto quantum : grants) {
            ++counts.scenarios;
            counts.boundaries += scenario(
                device,
                {.point = point, .sp = sp, .flags = static_cast<Byte>(flags), .failure = failure},
                quantum);
            require(
                device.core.statistics().compiled_entries == compiled,
                "Real entry PCs must retain their code across data, flags, refusals and grants");
          }
        }
      }
    }
  }
  require(device.code == original, "WAI owner traffic must leave original program storage intact");
  return counts;
}

struct StopDevice {
  std::array<Byte, address_bytes> ram{};
  std::array<Ram, 3> regions;
  Core core;

  explicit StopDevice(const std::span<const Byte> code)
      : regions{Ram{.bytes = std::span(ram).first(entry)},
                Ram{.first = entry,
                    .bytes = std::span(ram).subspan(entry, code.size()),
                    .writable = false},
                Ram{.first = static_cast<Address>(entry + code.size()),
                    .bytes = std::span(ram).subspan(entry + code.size())}},
        core(Memory::mapped(regions)) {
    ram.fill(sentinel);
    std::ranges::copy(code, ram.begin() + entry);
  }
};

void check_stop(StopDevice &device, const Scenario sample, const unsigned grant, const bool quiet) {
  const auto original = device.ram;
  const Registers initial{.pc = entry,
                          .d = marker,
                          .x = limit,
                          .y = sample.point.count,
                          .sp = sample.sp,
                          .ccr = sample.flags};
  device.core.set_registers(initial);
  auto expected = initial;
  auto image = original;
  const auto prefix = unsigned{initial.y} * 2;
  const auto progress = std::min(grant, prefix);
  const auto decrements = quiet ? progress / 2 : (progress + 1) / 2;
  expected.y = static_cast<Address>(unsigned{initial.y} - decrements);
  if (!quiet && progress != 0) {
    expected.ccr = static_cast<Byte>((initial.ccr & ~4U) | (expected.y == 0 ? 4U : 0U));
  }
  const auto stop = static_cast<Address>(entry + (quiet ? 4 : 3));
  expected.pc = progress == prefix ? stop : static_cast<Address>(entry + (progress % 2));
  const bool terminal = grant > prefix;
  constexpr Byte disable_stop = 0x80;
  const bool stopped = terminal && (expected.ccr & disable_stop) == 0;
  if (terminal) { expected.pc = static_cast<Address>(stop + isa::stop.size()); }
  if (stopped) {
    const std::array<Byte, frame_bytes> frame{expected.ccr,
                                              static_cast<Byte>(expected.d & byte_mask),
                                              static_cast<Byte>(expected.d >> byte_bits),
                                              static_cast<Byte>(expected.x >> byte_bits),
                                              static_cast<Byte>(expected.x & byte_mask),
                                              static_cast<Byte>(expected.y >> byte_bits),
                                              static_cast<Byte>(expected.y & byte_mask),
                                              static_cast<Byte>(expected.pc >> byte_bits),
                                              static_cast<Byte>(expected.pc & byte_mask)};
    expected.sp = wrap(unsigned{expected.sp} + word_values - frame_bytes);
    for (unsigned index = 0; index < frame.size(); ++index) {
      image[wrap(unsigned{expected.sp} + index)] = frame[index];
    }
  }
  const auto execution = stopped ? ExecutionState::stopped : ExecutionState::running;
  const auto last = terminal ? stop : static_cast<Address>(entry + (grant % 2 == 0 ? 1 : 0));
  const RunResult wanted{.retired = grant,
                         .state = execution,
                         .pc = grant == 0 ? entry : last,
                         .opcode = grant == 0 ? Byte{0} : original[last]};
  const State state{.registers = expected, .failure = {}, .execution = execution};
  const auto actual = device.core.run(grant);
  if (actual != wanted || device.core.state() != state || device.ram != image) {
    std::cerr << "quiet=" << quiet << " flags=" << unsigned{sample.flags}
              << " count=" << sample.point.count << " sp=" << sample.sp << " grant=" << grant
              << " pc=" << actual.pc << '/' << wanted.pc << " op=" << unsigned{actual.opcode} << '/'
              << unsigned{wanted.opcode} << " retired=" << actual.retired << '/' << wanted.retired
              << " state=" << (device.core.state() == state) << " ram=" << (device.ram == image)
              << " ccr=" << unsigned{device.core.registers().ccr} << '/' << unsigned{expected.ccr}
              << " y=" << device.core.registers().y << '/' << expected.y << '\n';
  }
  require(actual == wanted && device.core.state() == state && device.ram == image,
          "A fused STOP must preserve exact grants, S-bit, CCR, frame and full RAM");
  device.ram = original;
}

unsigned stop_cases() {
  constexpr std::array<Byte, 1> backedge{0xfd};
  constexpr std::array<Byte, 2> counted_backedge{0x36, 0xfc};
  constexpr auto dey =
      isa::program(isa::dey, isa::opcode<Opcode::bne_relative>(), backedge, isa::stop);
  constexpr auto dbne =
      isa::program(isa::nop, isa::opcode<Opcode::dbne_relative>(), counted_backedge, isa::stop);
  constexpr std::array<Address, 3> counts{1, 2, 3};
  constexpr std::array<Address, 2> stacks{0xffff, 0x9000};
  unsigned checked = 0;
  for (const bool quiet : {false, true}) {
    StopDevice device(quiet ? std::span<const Byte>(dbne) : std::span<const Byte>(dey));
    for (unsigned flags = 0; flags <= byte_mask; ++flags) {
      for (const auto count : counts) {
        const unsigned prefix = unsigned{count} * 2;
        const std::array grants{0U, prefix - 1, prefix, prefix + 1};
        for (const auto sp : stacks) {
          for (const auto grant : grants) {
            check_stop(device,
                       {.point = {.value = 0, .count = count},
                        .sp = sp,
                        .flags = static_cast<Byte>(flags),
                        .failure = phases},
                       grant,
                       quiet);
            ++checked;
          }
        }
      }
    }
  }
  return checked;
}

}

int main() {
  try {
    Counts counts;
    constexpr std::array modes{Mode{},
                               Mode{.dbne = true},
                               Mode{.simple = true},
                               Mode{.dbne = true, .simple = true},
                               Mode{.dbne = true, .simple = true, .quiet = true}};
    for (const auto mode : modes) {
      const auto result = cases(mode);
      counts.scenarios += result.scenarios;
      counts.boundaries += result.boundaries;
    }
    const auto stops = stop_cases();
    std::cout << stops << " independent loop/STOP S-bit and frame boundaries and "
              << counts.scenarios << " region/loop/WAI owner scenarios and " << counts.boundaries
              << " exact instruction, partial frame and failure boundaries passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
