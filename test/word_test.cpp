// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "reference.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace cpu12::test;
constexpr unsigned word_range = 65536;
constexpr unsigned word_mask = 0xffff;
constexpr unsigned word_sign = 0x8000;
constexpr unsigned byte_bits = 8;
constexpr std::uint16_t direct_address = 0x00ff;
constexpr std::uint16_t extended_address = 0xfffe;
constexpr std::uint16_t port_address = 0x1200;
std::uint64_t checked = 0;

void require(const bool condition, const std::string_view message) {
  if (!condition) { throw std::runtime_error(std::string(message)); }
}
enum class Function : std::uint8_t { load, add, subtract, compare, store };
enum class Form : std::uint8_t { immediate, direct, extended };
using Field = std::uint16_t cpu12::Registers::*;

struct Operation {
  std::uint8_t opcode;
  Field field;
  Function function;
};

constexpr std::array<Operation, 10> operations{
    {{.opcode = 0xcc, .field = &cpu12::Registers::d, .function = Function::load},
     {.opcode = 0xcd, .field = &cpu12::Registers::y, .function = Function::load},
     {.opcode = 0xce, .field = &cpu12::Registers::x, .function = Function::load},
     {.opcode = 0xcf, .field = &cpu12::Registers::sp, .function = Function::load},
     {.opcode = 0x8c, .field = &cpu12::Registers::d, .function = Function::compare},
     {.opcode = 0x8d, .field = &cpu12::Registers::y, .function = Function::compare},
     {.opcode = 0x8e, .field = &cpu12::Registers::x, .function = Function::compare},
     {.opcode = 0x8f, .field = &cpu12::Registers::sp, .function = Function::compare},
     {.opcode = 0xc3, .field = &cpu12::Registers::d, .function = Function::add},
     {.opcode = 0x83, .field = &cpu12::Registers::d, .function = Function::subtract}}};
constexpr std::array<Operation, 4> stores{
    {{.opcode = 0x5c, .field = &cpu12::Registers::d, .function = Function::store},
     {.opcode = 0x5d, .field = &cpu12::Registers::y, .function = Function::store},
     {.opcode = 0x5e, .field = &cpu12::Registers::x, .function = Function::store},
     {.opcode = 0x5f, .field = &cpu12::Registers::sp, .function = Function::store}}};
constexpr std::array<unsigned, 8> partners{0, 1, 0xff, 0x100, 0x7fff, 0x8000, 0xff00, 0xffff};

struct Case {
  Operation operation;
  Form form;
  unsigned left, right;
  std::uint16_t address;
  std::uint8_t ccr;
};

struct Encoding {
  std::array<std::uint8_t, 3> code{};
  std::size_t size = 0;

  [[nodiscard]] std::span<const std::uint8_t> view() const { return std::span(code).first(size); }
};

Encoding encode(const Case &test) {
  auto opcode = static_cast<unsigned>(test.operation.opcode);
  constexpr unsigned direct_delta = 0x10;
  constexpr unsigned extended_delta = 0x30;
  constexpr unsigned store_delta = 0x20;
  unsigned operand = test.address;
  if (test.operation.function == Function::store) {
    require(test.form != Form::immediate, "The test must not fabricate an immediate store");
    if (test.form == Form::extended) { opcode += store_delta; }
  } else if (test.form == Form::immediate) {
    operand = test.right;
  } else if (test.form == Form::direct) {
    opcode += direct_delta;
  } else {
    opcode += extended_delta;
  }
  const auto low = static_cast<std::uint8_t>(operand & byte_mask);
  const auto high = static_cast<std::uint8_t>(operand >> byte_bits);
  return {.code = {static_cast<std::uint8_t>(opcode), test.form == Form::direct ? low : high, low},
          .size = test.form == Form::direct ? 2U : 3U};
}

int signed_word(const unsigned value) {
  return value < word_sign ? static_cast<int>(value)
                           : static_cast<int>(value) - static_cast<int>(word_range);
}

struct Prediction {
  unsigned value, ccr;
};

Prediction predict(const Case &test) {
  unsigned value = test.right;
  unsigned flags = 0;
  unsigned changed = nzv;
  const auto function = test.operation.function;
  if (function == Function::store) { value = test.left; }
  if (function == Function::add) {
    const auto sum = test.left + test.right;
    value = sum % word_range;
    const auto signed_sum = signed_word(test.left) + signed_word(test.right);
    flags = (sum >= word_range ? carry : 0U) |
            (signed_sum < -static_cast<int>(word_sign) || signed_sum >= static_cast<int>(word_sign)
                 ? overflow
                 : 0U);
    changed = nzvc;
  } else if (function == Function::subtract || function == Function::compare) {
    const auto difference = static_cast<int>(test.left) - static_cast<int>(test.right);
    value = static_cast<unsigned>(difference + static_cast<int>(word_range)) % word_range;
    const auto signed_difference = signed_word(test.left) - signed_word(test.right);
    flags =
        (difference < 0 ? carry : 0U) | (signed_difference < -static_cast<int>(word_sign) ||
                                                 signed_difference >= static_cast<int>(word_sign)
                                             ? overflow
                                             : 0U);
    changed = nzvc;
  }
  flags |= (value == 0 ? zero : 0U) | (value >= word_sign ? negative : 0U);
  return {.value = value, .ccr = (test.ccr & (byte_mask ^ changed)) | flags};
}

void place(std::span<std::uint8_t> ram, const std::uint16_t address, const unsigned value) {
  ram[address] = static_cast<std::uint8_t>(value >> byte_bits);
  ram[static_cast<std::uint16_t>(static_cast<unsigned>(address) + 1U)] =
      static_cast<std::uint8_t>(value & byte_mask);
}

void run_case(cpu12::Core &core, const Case &test) {
  auto before = seed;
  before.*test.operation.field = static_cast<std::uint16_t>(test.left);
  before.ccr = test.ccr;
  auto wanted = before;
  const auto instruction = encode(test);
  const auto prediction = predict(test);
  wanted.pc = static_cast<std::uint16_t>(wanted.pc + instruction.size);
  wanted.ccr = static_cast<std::uint8_t>(prediction.ccr);
  if (test.operation.function != Function::compare && test.operation.function != Function::store) {
    wanted.*test.operation.field = static_cast<std::uint16_t>(prediction.value);
  }
  core.set_registers(before);
  const auto result = core.run_instruction(instruction.view());
  require(result.retired == 1 && result.fault == cpu12::Fault::none && result.pc == before.pc &&
              core.registers() == wanted,
          "Independent word arithmetic/target/flags/PC expectation failed");
  ++checked;
}

void arithmetic_matrix(cpu12::Core &core, std::span<std::uint8_t> ram, const Form form) {
  const auto address = form == Form::direct ? direct_address : extended_address;
  for (const auto &operation : operations) {
    for (const auto right : partners) {
      place(ram, address, right);
      const unsigned limit = operation.function == Function::load ? 1U : word_range;
      for (unsigned left = 0; left < limit; ++left) {
        const auto ccr = static_cast<std::uint8_t>((left & 1U) != 0 ? byte_mask : 0U);
        run_case(core,
                 {.operation = operation,
                  .form = form,
                  .left = left,
                  .right = right,
                  .address = address,
                  .ccr = ccr});
      }
    }
  }
}

void load_store_matrix(cpu12::Core &core, std::span<std::uint8_t> ram, const Form form) {
  const auto address = form == Form::direct ? direct_address : extended_address;
  for (unsigned value = 0; value < word_range; ++value) {
    place(ram, address, value);
    for (const auto &operation : operations) {
      if (operation.function == Function::load) {
        run_case(core,
                 {.operation = operation,
                  .form = form,
                  .left = word_mask,
                  .right = value,
                  .address = address,
                  .ccr = byte_mask});
      }
    }
    for (const auto &operation : stores) {
      place(ram, address, 0);
      run_case(core,
               {.operation = operation,
                .form = form,
                .left = value,
                .right = 0,
                .address = address,
                .ccr = byte_mask});
      require(ram[address] == value >> byte_bits &&
                  ram[static_cast<std::uint16_t>(static_cast<unsigned>(address) + 1U)] ==
                      (value & byte_mask),
              "RAM word stores retain big-endian byte order");
    }
  }
}

struct WordBus {
  std::uint16_t address = port_address, value = 0;
  std::uint8_t committed_high = 0;
  unsigned reads = 0, writes = 0, byte_calls = 0;
  bool refuse_read = false, refuse_write = false;
  std::uint8_t *ram_high = nullptr;
  static bool read(void *owner, std::uint16_t address, std::uint16_t &value) noexcept;
  static bool write(void *owner, cpu12::WordWrite access) noexcept;
  static bool byte_read(void *owner, std::uint16_t address, std::uint8_t &value) noexcept;
};

bool WordBus::read(void *const owner, const std::uint16_t address, std::uint16_t &value) noexcept {
  auto &bus = *static_cast<WordBus *>(owner);
  ++bus.reads;
  value = bus.value;
  if (bus.ram_high != nullptr) {
    value = static_cast<std::uint16_t>((static_cast<unsigned>(*bus.ram_high) << byte_bits) |
                                       (bus.value & byte_mask));
  }
  return address == bus.address && !bus.refuse_read;
}

bool WordBus::write(void *const owner, const cpu12::WordWrite access) noexcept {
  auto &bus = *static_cast<WordBus *>(owner);
  ++bus.writes;
  if (access.address != bus.address) { return false; }
  bus.committed_high = static_cast<std::uint8_t>(access.value >> byte_bits);
  if (bus.ram_high != nullptr) { *bus.ram_high = bus.committed_high; }
  if (bus.refuse_write) { return false; }
  bus.value = access.value;
  return true;
}

bool WordBus::byte_read(void *const owner,
                        const std::uint16_t /*address*/,
                        std::uint8_t & /*value*/) noexcept {
  ++static_cast<WordBus *>(owner)->byte_calls;
  return false;
}

void bus_contract() {
  WordBus bus;
  cpu12::Core core({},
                   {.owner = &bus,
                    .read = &WordBus::byte_read,
                    .read_word = &WordBus::read,
                    .write_word = &WordBus::write});
  for (const auto &operation : operations) {
    for (const auto right : partners) {
      bus.value = static_cast<std::uint16_t>(right);
      const auto calls = bus.reads;
      run_case(core,
               {.operation = operation,
                .form = Form::extended,
                .left = word_sign - 1,
                .right = right,
                .address = bus.address,
                .ccr = byte_mask});
      require(bus.reads == calls + 1 && bus.byte_calls == 0,
              "One logical word read invokes one wide callback, never implicit byte callbacks");
    }
  }
  for (const auto &operation : stores) {
    const auto calls = bus.writes;
    run_case(core,
             {.operation = operation,
              .form = Form::extended,
              .left = word_sign + 1,
              .right = 0,
              .address = bus.address,
              .ccr = byte_mask});
    require(bus.writes == calls + 1 && bus.value == word_sign + 1,
            "Word stores use their selected register and exactly one logical write");
  }
  core.set_registers(seed);
  bus.refuse_write = true;
  const auto instruction = encode({.operation = stores[0],
                                   .form = Form::extended,
                                   .left = seed.d,
                                   .right = 0,
                                   .address = bus.address,
                                   .ccr = seed.ccr});
  const auto failure = core.run_instruction(instruction.view());
  const auto calls = bus.writes;
  require(
      failure.fault == cpu12::Fault::memory_write && failure.address == bus.address &&
          failure.access_size == 2 && failure.value == seed.d && failure.pc == seed.pc &&
          failure.retired == 0 && bus.committed_high == seed.d >> byte_bits &&
          core.registers() == seed,
      "A failed wide write retains its real partial effect, full payload and unchanged CPU state");
  require(core.run_instruction(instruction.view()).retired == 0 && bus.writes == calls,
          "Failed word writes latch instead of replaying partial transfers");
  core.set_registers(seed);
  bus.refuse_read = true;
  const auto load = encode({.operation = operations[0],
                            .form = Form::extended,
                            .left = 0,
                            .right = 0,
                            .address = bus.address,
                            .ccr = 0});
  const auto read = core.run_instruction(load.view());
  require(read.fault == cpu12::Fault::memory_read && read.access_size == 2 &&
              read.address == bus.address && read.retired == 0 && core.registers() == seed,
          "Failed word reads cannot commit flags or destination registers");
  cpu12::Core missing({}, {.owner = &bus, .read = &WordBus::byte_read});
  missing.set_registers(seed);
  require(missing.run_instruction(load.view()).fault == cpu12::Fault::memory_read &&
              bus.byte_calls == 0,
          "Missing wide handlers refuse rather than guessing byte-access equivalence");
}

void boundaries_and_keys() {
  std::array<std::uint8_t, word_range> ram{};
  cpu12::Core core({.bytes = ram});
  constexpr std::uint16_t last = 0xffff;
  constexpr unsigned pattern = 0x1234;
  place(ram, last, pattern);
  run_case(core,
           {.operation = operations[0],
            .form = Form::extended,
            .left = 0,
            .right = pattern,
            .address = last,
            .ccr = byte_mask});
  run_case(core,
           {.operation = stores[2],
            .form = Form::extended,
            .left = word_mask,
            .right = 0,
            .address = last,
            .ccr = 0});
  require(
      ram[last] == byte_mask && ram[0] == byte_mask && core.statistics().read_callbacks == 0 &&
          core.statistics().write_callbacks == 0,
      "Known RAM word accesses wrap ffff to zero without out-of-range host memory or callbacks");
  constexpr std::array<unsigned, 4> immediates{0x1234, 0xab34, 0x12cd, 0x1234};
  core.clear_code_cache();
  const auto count = core.statistics().compiled_entries;
  for (const auto value : immediates) {
    run_case(core,
             {.operation = operations[0],
              .form = Form::immediate,
              .left = 0,
              .right = value,
              .address = 0,
              .ccr = 0});
  }
  require(core.statistics().compiled_entries == count + 3,
          "Immediate word cache keys retain both operand bytes");
  std::array<std::uint8_t, 1> high{static_cast<std::uint8_t>(pattern >> byte_bits)};
  WordBus bus;
  bus.value = pattern;
  bus.ram_high = high.data();
  cpu12::Core mixed({.first = port_address, .bytes = high},
                    {.owner = &bus, .read_word = &WordBus::read, .write_word = &WordBus::write});
  run_case(mixed,
           {.operation = operations[0],
            .form = Form::extended,
            .left = 0,
            .right = pattern,
            .address = port_address,
            .ccr = 0});
  require(bus.reads == 1,
          "A word crossing the fast RAM window delegates the complete logical access");
  run_case(mixed,
           {.operation = stores[0],
            .form = Form::extended,
            .left = word_sign,
            .right = 0,
            .address = port_address,
            .ccr = 0});
  require(bus.writes == 1 && high[0] == sign_bit && bus.value == word_sign,
          "The wide bus owner resolves a mixed RAM/peripheral transfer");
}
}

int main() {
  try {
    require(cpu12::Core::native_supported(), "Native host required");
    std::array<std::uint8_t, word_range> ram{};
    cpu12::Core core({.bytes = ram});
    arithmetic_matrix(core, ram, Form::immediate);
    arithmetic_matrix(core, ram, Form::direct);
    arithmetic_matrix(core, ram, Form::extended);
    load_store_matrix(core, ram, Form::direct);
    load_store_matrix(core, ram, Form::extended);
    require(core.statistics().native_instructions == checked &&
                core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
            "All matrix retirements actually use native RAM/register code");
    bus_contract();
    boundaries_and_keys();
    std::cout << "Verified " << checked
              << " native word cases plus wide-bus, fault, endian, wrap and cache contracts.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
