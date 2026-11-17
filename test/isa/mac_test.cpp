// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address entry = 0x8000, accumulator = 0x200, left = 0x100, right = 0x110;
constexpr unsigned byte_bits = 8, word_bits = 16, word_mask = 0xffff, unchanged = 0xf0;
constexpr unsigned sign_bit = 0x80000000;
constexpr std::array<Byte, 6> program{0x18, 0x12, 2, 0, 0x20, 0xfe};

void require(const bool good, const char *reason) {
  if (!good) { throw std::runtime_error(reason); }
}

void put(std::span<Byte> ram, const Address address, const unsigned value) {
  ram[address] = static_cast<Byte>(value >> byte_bits);
  ram[static_cast<Address>(address + 1)] = static_cast<Byte>(value);
}

unsigned get(std::span<const Byte> ram, const Address address) {
  return (unsigned{ram[address]} << byte_bits) | ram[static_cast<Address>(address + 1)];
}

struct Bus {
  std::array<Byte, address_bytes> ram{};
  unsigned accesses = 0, refuse = 0;
  bool yield = false;
  Core *core = nullptr;
  std::vector<Address> addresses;

  static bool read(void *owner, const Address address, std::uint16_t &value) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    self.addresses.push_back(address);
    if (++self.accesses == self.refuse) { return false; }
    value = static_cast<std::uint16_t>(get(self.ram, address));
    if (self.yield) { self.core->request_yield(); }
    return true;
  }

  static bool write(void *owner, const WordWrite request) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    self.addresses.push_back(request.address);
    if (++self.accesses == self.refuse) { return false; }
    put(self.ram, request.address, request.value);
    if (self.yield) { self.core->request_yield(); }
    return true;
  }
};

struct Input {
  std::uint32_t accumulator;
  std::uint16_t a, b;
  Byte ccr = 0;
};

class Fixture {
public:
  explicit Fixture(const bool external, const bool dynamic) {
    bus.addresses.reserve(6);
    std::ranges::copy(program, bus.ram.begin() + entry);
    const auto end = static_cast<Address>(entry + program.size());
    regions = {Ram{.bytes = std::span(bus.ram).first(entry)},
               Ram{.first = entry,
                   .bytes = std::span(bus.ram).subspan(entry, program.size()),
                   .writable = false},
               Ram{.first = end, .bytes = std::span(bus.ram).subspan(end)}};
    const auto memory = external ? Memory::window(regions[1]) : Memory::mapped(regions);
    const std::array code{CodeSpan{.address = entry, .words = program}};
    constexpr std::array entries{entry};
    const std::array variants{CodeVariant{.id = 0, .code = code, .entries = entries}};
    std::vector<RegionLayout> layout;
    layout.reserve(regions.size());
    for (const auto &region : memory.regions()) {
      layout.push_back(
          {.first = region.first, .size = region.bytes.size(), .writable = region.writable});
    }
    const std::array layouts{VariantLayout{.id = 0, .memory = layout}};
    const auto image =
        Core::compile({.variants = variants},
                      {.memory = dynamic ? std::span<const VariantLayout>{}
                                         : std::span<const VariantLayout>(layouts),
                       .interrupts = {},
                       .transitions = {},
                       .callbacks = {},
                       .bindings = {},
                       .topology = dynamic ? MemoryTopology::dynamic : MemoryTopology::fixed});
    core.set_memory(memory);
    core.install(image, 0);
    bus.core = &core;
  }

  void reset(const Input input) {
    bus.accesses = 0;
    bus.addresses.clear();
    put(bus.ram, left, input.a);
    put(bus.ram, right, input.b);
    put(bus.ram, accumulator, input.accumulator >> word_bits);
    put(bus.ram, accumulator + 2, input.accumulator);
    constexpr Address untouched_d = 0x5678;
    constexpr Address stack = 0x7000;
    core.set_registers(
        {.pc = entry, .d = untouched_d, .x = left, .y = right, .sp = stack, .ccr = input.ccr});
  }

  [[nodiscard]] std::uint32_t result() const {
    return (get(bus.ram, accumulator) << word_bits) | get(bus.ram, accumulator + 2);
  }

  Bus bus;
  Io io{.owner = &bus, .read_word = Bus::read, .write_word = Bus::write};
  Core core{Memory::external(), &io};
  std::array<Ram, 3> regions;
};

void check_arithmetic(Fixture &f, const Input input) {
  constexpr std::int64_t minimum = -2147483648LL;
  constexpr std::int64_t maximum = 2147483647;
  f.reset(input);
  const auto before = f.core.registers();
  const auto cycles = f.core.progress().estimated_cycles;
  const auto product =
      std::int64_t{std::bit_cast<std::int16_t>(input.a)} * std::bit_cast<std::int16_t>(input.b);
  const auto sum = std::int64_t{std::bit_cast<std::int32_t>(input.accumulator)} + product;
  const auto result = static_cast<std::uint32_t>(sum);
  const auto low_sum =
      (input.accumulator & word_mask) + (static_cast<std::uint32_t>(product) & word_mask);
  const unsigned flags = (input.ccr & unchanged) | (sum < minimum || sum > maximum ? 2U : 0U) |
                         (result == 0 ? 4U : 0U) | ((result & sign_bit) != 0 ? 8U : 0U) |
                         (low_sum > word_mask ? 1U : 0U);
  auto wanted = before;
  wanted.pc += 4;
  wanted.ccr = static_cast<Byte>(flags);
  const auto run = f.core.run(1);
  require(run.fault == Fault::none && run.retired == 1 && f.core.registers() == wanted &&
              f.result() == result && f.core.progress().estimated_cycles == cycles + 13,
          "EMACS result, flags, preserved registers or nominal cycles differ");
}

void arithmetic(const bool dynamic) {
  Fixture f(false, dynamic);
  constexpr std::array<std::uint16_t, 8> operands{0, 1, 2, 0x7fff, 0x8000, 0x8001, 0xfffe, 0xffff};
  constexpr std::array<std::uint32_t, 10> initial{
      0, 1, 0x7fff, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0x80000001, 0xffff0000, 0xffffffff};
  for (const auto a : operands) {
    for (const auto b : operands) {
      for (const auto old : initial) {
        for (unsigned ccr = 0; ccr <= std::numeric_limits<Byte>::max(); ++ccr) {
          check_arithmetic(f, {.accumulator = old, .a = a, .b = b, .ccr = static_cast<Byte>(ccr)});
        }
      }
    }
  }
}

void transactions() {
  Fixture f(true, false);
  constexpr std::uint32_t initial = 0x1234ffff;
  constexpr std::uint32_t result = 0x12350001;
  constexpr std::uint32_t partial_write = 0x1235ffff;
  constexpr std::array<Address, 6> order{
      left, right, accumulator, accumulator + 2, accumulator, accumulator + 2};
  for (unsigned refusal = 0; refusal <= order.size(); ++refusal) {
    f.bus.refuse = refusal;
    f.reset({.accumulator = initial, .a = 1, .b = 2});
    const auto before = f.core.registers();
    const auto cycles = f.core.progress().estimated_cycles;
    const auto run = f.core.run(1);
    const auto count = refusal == 0 ? order.size() : refusal;
    require(std::ranges::equal(f.bus.addresses, std::span(order).first(count)),
            "EMACS must read X, Y, accumulator high/low, then write high/low");
    if (refusal == 0) {
      require(run.retired == 1 && run.fault == Fault::none && f.result() == result,
              "External EMACS must complete its exact result");
    } else {
      require(run.retired == 0 &&
                  run.fault == (refusal > 4 ? Fault::memory_write : Fault::memory_read) &&
                  run.address == order[refusal - 1] && run.access_size == 2 &&
                  f.core.registers() == before && f.core.progress().estimated_cycles == cycles &&
                  f.result() == (refusal == 6 ? partial_write : initial),
              "Refused EMACS must preserve its exact committed memory prefix");
    }
  }
  f.bus.refuse = 0;
  f.bus.yield = true;
  f.reset({.accumulator = initial, .a = 1, .b = 2});
  const auto run = f.core.run();
  require(run.retired == 1 && run.fault == Fault::none && f.result() == result &&
              f.bus.accesses == order.size(),
          "Callback yield must finish the complete EMACS");
}

void aliases_and_bounds(const bool dynamic) {
  Fixture f(false, dynamic);
  constexpr std::uint32_t initial = 0x1234ffff;
  constexpr std::array<Address, 5> sources{left, accumulator, accumulator + 2, 0xfffe, 0xffff};
  for (const auto x : sources) {
    for (const auto y : sources) {
      for (unsigned deadline = 1; deadline <= 13; ++deadline) {
        f.reset({.accumulator = initial, .a = 1, .b = 2});
        constexpr Address wrapping_source = 0xffff;
        constexpr Address wrapped_word = 0x8001;
        put(f.bus.ram, wrapping_source, wrapped_word);
        auto registers = f.core.registers();
        registers.x = x;
        registers.y = y;
        f.core.set_registers(registers);
        const auto a = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(get(f.bus.ram, x)));
        const auto b = std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(get(f.bus.ram, y)));
        const auto result =
            static_cast<std::uint32_t>(std::int64_t{initial} + (std::int64_t{a} * b));
        auto expected = f.bus.ram;
        put(expected, accumulator, result >> word_bits);
        put(expected, accumulator + 2, result);
        const auto cycles = f.core.progress().estimated_cycles;
        const auto run = f.core.run_until(cycles + deadline);
        require(run.fault == Fault::none && run.retired == 1 && f.bus.ram == expected &&
                    f.core.progress().estimated_cycles == cycles + 13,
                "EMACS must use old aliased operands, wrap words and cross exact cycle bounds");
      }
    }
  }
}

void deadline_overflow() {
  Fixture f(true, false);
  f.bus.yield = true;
  f.reset({.accumulator = 0, .a = 1, .b = 2});
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  auto saved = f.core.save();
  saved.progress.estimated_cycles = maximum - 13;
  f.core.restore(saved);
  const auto complete = f.core.run_until(maximum - 12);
  require(complete.retired == 1 && complete.fault == Fault::none && f.result() == 2 &&
              f.core.progress().estimated_cycles == maximum,
          "EMACS may complete at the last representable nominal cycle");
  f.reset({.accumulator = 0, .a = 1, .b = 2});
  saved = f.core.save();
  saved.progress.estimated_cycles = maximum - 12;
  f.core.restore(saved);
  const auto memory = f.bus.ram;
  bool rejected = false;
  try {
    static_cast<void>(f.core.run_until(maximum - 11));
  } catch (const std::overflow_error &) { rejected = true; }
  require(rejected && f.core.save() == saved && f.bus.ram == memory && f.bus.accesses == 0,
          "A deadline without room for EMACS must reject before state, I/O or time changes");
}
}

int main() {
  try {
    arithmetic(false);
    arithmetic(true);
    transactions();
    aliases_and_bounds(false);
    aliases_and_bounds(true);
    deadline_overflow();
    std::cout
        << "EMACS arithmetic, CCR, fixed/dynamic storage and ordered fault/yield prefixes pass\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
