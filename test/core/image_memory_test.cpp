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
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace cpu12;
constexpr Address entry = 0x8000, data = 0x1200;
constexpr Byte nzv = 0x0e, negative = 0x08;
constexpr unsigned byte_bits = 8, instruction_cycles = 3;
constexpr Byte backedge = 0xfe;
constexpr Address positive_value = 0x1234, negative_value = 0x80f0, failed_value = 0x4455;
constexpr Registers seed{.pc = entry, .d = 0xabcd, .x = 0x3456, .y = 0x789a, .sp = 0x4321};
enum class Layout : Byte { tracked, plain, split, read_only, unknown };

struct Scenario {
  Layout layout;
  Address value;
  unsigned retired;
  Fault fault = Fault::none;
};

constexpr std::array scenarios{
    Scenario{.layout = Layout::tracked, .value = positive_value, .retired = 2},
    Scenario{.layout = Layout::plain, .value = negative_value, .retired = 2},
    Scenario{.layout = Layout::split,
             .value = positive_value,
             .retired = 1,
             .fault = Fault::memory_write},
    Scenario{.layout = Layout::read_only,
             .value = positive_value,
             .retired = 1,
             .fault = Fault::memory_write},
    Scenario{.layout = Layout::unknown,
             .value = positive_value,
             .retired = 0,
             .fault = Fault::memory_read}};
constexpr auto program = isa::program(isa::opcode<isa::Opcode::ldd_extended>(),
                                      std::array<Byte, 2>{0x12, 0x00},
                                      isa::opcode<isa::Opcode::std_extended>(),
                                      std::array<Byte, 2>{0x12, 0x02},
                                      isa::opcode<isa::Opcode::bra_relative>(),
                                      std::array<Byte, 1>{backedge});

void require(const bool value, const char *reason) {
  if (!value) { throw std::runtime_error(reason); }
}

NativeImage prepare(const std::span<const Byte> input = program) {
  constexpr std::array roots{entry};
  const std::array code{CodeSpan{.address = entry, .words = input}};
  const std::array variants{CodeVariant{.id = 1, .code = code, .entries = roots}};
  return Core::compile({.variants = variants},
                       {.memory = {},
                        .interrupts = {},
                        .transitions = {},
                        .callbacks = {},
                        .bindings = {},
                        .topology = MemoryTopology::dynamic});
}

struct Bus {
  std::array<Byte, program.size()> code = program;
  std::array<Byte, 8> bytes{}, known{};
  std::array<Byte, 2> extra{};
  Layout mode = Layout::tracked;
  unsigned reads = 0, writes = 0;
  Address read_address = 0;
  WordWrite transaction{};

  std::vector<Ram> reset(const Scenario scenario) {
    mode = scenario.layout;
    read_address = 0;
    reads = writes = 0;
    transaction = {};
    bytes.fill(0);
    known.fill(mode == Layout::plain ? 1 : 0);
    const unsigned offset = mode == Layout::plain ? 1 : 0;
    bytes[offset] = static_cast<Byte>(scenario.value >> byte_bits);
    bytes[offset + 1] = static_cast<Byte>(scenario.value);
    known[offset] = mode == Layout::unknown ? 0 : 1;
    known[offset + 1] = 1;
    std::vector<Ram> grants{{.first = entry, .bytes = code, .writable = false}};
    if (mode == Layout::plain) {
      grants.push_back({.first = data - 1, .bytes = std::span(bytes).first(6)});
      grants.push_back({.first = 0x2000, .bytes = extra});
    } else if (mode == Layout::split) {
      grants.push_back(
          {.first = data, .bytes = std::span(bytes).first(3), .known = std::span(known).first(3)});
      grants.push_back({.first = data + 3,
                        .bytes = std::span(bytes).subspan(3, 1),
                        .known = std::span(known).subspan(3, 1)});
    } else {
      grants.push_back({.first = data,
                        .bytes = std::span(bytes).first(4),
                        .known = std::span(known).first(4),
                        .writable = mode != Layout::read_only});
    }
    return grants;
  }

  static bool read(void *owner, const Address address, std::uint16_t &value) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    ++self.reads;
    self.read_address = address;
    value = failed_value;
    return false;
  }

  static bool write(void *owner, const WordWrite access) noexcept {
    auto &self = *static_cast<Bus *>(owner);
    ++self.writes;
    self.transaction = access;
    if (self.mode == Layout::split) {
      self.bytes[2] = static_cast<Byte>(access.value >> 8U);
      self.known[2] = 1;
    }
    return false;
  }
};

struct Expected {
  State state;
  std::array<Byte, 8> bytes, known;
};

Expected predict(const Bus &bus, const Registers before, const Scenario scenario) {
  Expected expected{
      .state = {.registers = before, .failure = {}}, .bytes = bus.bytes, .known = bus.known};
  if (scenario.retired != 0) {
    expected.state.registers.d = scenario.value;
    expected.state.registers.ccr =
        static_cast<Byte>((unsigned{before.ccr} & ~unsigned{nzv}) |
                          (scenario.layout == Layout::plain ? negative : 0));
    expected.state.registers.pc = static_cast<Address>(entry + (3U * scenario.retired));
  }
  const auto offset = scenario.layout == Layout::plain ? 1U : 0U;
  if (scenario.fault == Fault::none || scenario.layout == Layout::split) {
    expected.bytes[offset + 2] = static_cast<Byte>(scenario.value >> byte_bits);
  }
  if (scenario.fault == Fault::none) {
    expected.bytes[offset + 3] = static_cast<Byte>(scenario.value);
  }
  if (scenario.layout == Layout::tracked || scenario.layout == Layout::split) {
    expected.known[2] = 1;
  }
  if (scenario.layout == Layout::tracked) { expected.known[3] = 1; }
  if (scenario.fault != Fault::none) {
    const bool read = scenario.fault == Fault::memory_read;
    expected.state.failure = {
        .fault = scenario.fault,
        .pc = static_cast<Address>(entry + (read ? 0U : 3U)),
        .opcode = static_cast<Byte>(read ? isa::Opcode::ldd_extended : isa::Opcode::std_extended),
        .address = static_cast<Address>(data + (read ? 0U : 2U)),
        .value = read ? failed_value : scenario.value,
        .access_size = 2};
  }
  return expected;
}

void check(
    Core &core, Bus &bus, const NativeImage &image, const Scenario scenario, const Byte flags) {
  const auto grants = bus.reset(scenario);
  core.set_memory(Memory::mapped(grants));
  auto before = seed;
  before.ccr = flags;
  core.set_registers(before);
  core.install(image, 1);
  const auto expected = predict(bus, before, scenario);
  const auto extra = bus.extra;
  const auto progress = core.progress().estimated_cycles;
  const auto translated = core.statistics().compiled_entries;
  const auto result = core.run(2);
  require(
      result.retired == scenario.retired && result.fault == scenario.fault &&
          core.state() == expected.state &&
          core.progress().estimated_cycles ==
              progress + (std::uint64_t{scenario.retired} * instruction_cycles) &&
          core.statistics().compiled_entries == translated,
      "Dynamic word remaps preserve independent full state and exact progress without translation");
  require(
      bus.bytes == expected.bytes && bus.known == expected.known && bus.extra == extra &&
          bus.code == program,
      "Dynamic word remaps preserve every byte and knownness, including actual partial I/O writes");
  const bool read_fault = scenario.fault == Fault::memory_read;
  const bool write_fault = scenario.fault == Fault::memory_write;
  require(bus.reads == static_cast<unsigned>(read_fault) &&
              bus.writes == static_cast<unsigned>(write_fault) &&
              bus.read_address == (read_fault ? data : 0),
          "Only real whole-word I/O faults invoke their exact external callback");
  if (write_fault) {
    require(bus.transaction.address == data + 2 && bus.transaction.value == scenario.value,
            "A failing word callback receives its original address and value exactly once");
  }
}

void remaps(const NativeImage &image) {
  Bus bus;
  const auto grants = bus.reset(scenarios.front());
  Io io{.owner = &bus, .read_word = Bus::read, .write_word = Bus::write};
  Core core(Memory::mapped(grants), &io);
  for (unsigned flags = 0; flags < 256; ++flags) {
    for (const auto scenario : scenarios) {
      check(core, bus, image, scenario, static_cast<Byte>(flags));
    }
  }
}

template <class Action> void rejected(Action action, const std::string_view diagnostic) {
  try {
    action();
  } catch (const std::invalid_argument &error) {
    require(std::string_view(error.what()).find(diagnostic) != std::string_view::npos,
            "Rejected dynamic binding preserves its exact diagnostic");
    return;
  }
  throw std::runtime_error("Invalid dynamic binding was accepted");
}

void admission(const NativeImage &image) {
  for (unsigned mode = 0; mode < 3; ++mode) {
    auto code = program;
    std::array<Byte, program.size()> known{};
    known.fill(1);
    if (mode == 1) { known[1] = 0; }
    if (mode == 2) { code[1] ^= 1; }
    Core core(
        Memory::window({.first = entry, .bytes = code, .known = known, .writable = mode == 0}));
    const auto before = core.state();
    const auto progress = core.progress();
    const auto bytes = code;
    const auto initialized = known;
    rejected([&] { core.install(image, 1); }, "executable source does not match");
    require(
        core.state() == before && core.progress() == progress && code == bytes &&
            known == initialized,
        "Dynamic admission rejects writable, unknown and mismatched code without state effects");
  }
  constexpr std::array roots{entry};
  const std::array code{CodeSpan{.address = entry, .words = program}};
  const std::array variants{CodeVariant{.id = 1, .code = code, .entries = roots}};
  const std::array regions{RegionLayout{.first = entry, .size = program.size(), .writable = false}};
  const std::array layouts{VariantLayout{.id = 1, .memory = regions}};
  rejected(
      [&] {
        static_cast<void>(Core::lower({.variants = variants},
                                      {.memory = layouts,
                                       .interrupts = {},
                                       .transitions = {},
                                       .callbacks = {},
                                       .bindings = {},
                                       .topology = MemoryTopology::dynamic}));
      },
      "cannot declare fixed memory layouts");
}

void wrapped_word() {
  auto code = program;
  constexpr Byte last_byte = 0xff;
  code[1] = code[2] = last_byte;
  const auto image = prepare(code);
  std::array<Byte, address_bytes> bytes{};
  std::ranges::copy(code, bytes.begin() + entry);
  bytes.back() = static_cast<Byte>(positive_value >> byte_bits);
  bytes.front() = static_cast<Byte>(positive_value);
  Core core(Memory::window({.bytes = bytes, .writable = false}));
  core.install(image, 1);
  const auto storage = bytes;
  for (unsigned flags = 0; flags < 256; ++flags) {
    auto expected = seed;
    expected.ccr = static_cast<Byte>(flags);
    core.set_registers(expected);
    const auto cycles = core.progress().estimated_cycles;
    expected.d = positive_value;
    expected.pc = entry + 3;
    expected.ccr = static_cast<Byte>(flags & ~unsigned{nzv});
    const auto result = core.run(1);
    require(
        result.retired == 1 && result.fault == Fault::none &&
            core.state() == State{.registers = expected, .failure = {}} &&
            core.progress().estimated_cycles == cycles + instruction_cycles && bytes == storage,
        "A dynamic full-address-space word load wraps FFFF to0000 in original big-endian order");
  }
}

struct Masked {
  Byte value = 0;
  bool accepted = true;
  unsigned calls = 0;
  MaskedByteRead seen{};

  static bool read(void *owner, const MaskedByteRead access, Byte &value) noexcept {
    auto &self = *static_cast<Masked *>(owner);
    ++self.calls;
    self.seen = access;
    value = self.value;
    return self.accepted;
  }
};

struct Prefix {
  unsigned mode;
  Byte flags;
};

void masked_prefix(Core &core, Masked &bus, Byte &known, const Prefix prefix) {
  const auto mode = prefix.mode;
  constexpr Byte mask = 0x80;
  constexpr Byte ignored_bits = 0x7f;
  constexpr unsigned branch_cycles = 5;
  known = mode < 2 ? 1 : 0;
  bus = {.value = static_cast<Byte>(mode % 2 == 0 ? ignored_bits : mask), .accepted = mode != 4};
  auto expected = seed;
  expected.ccr = prefix.flags;
  core.set_registers(expected);
  const auto before = core.progress().estimated_cycles;
  const bool fault = mode == 4;
  const bool taken = mode % 2 != 0;
  if (!fault) { expected.pc = static_cast<Address>(entry + (taken ? 6U : 5U)); }
  State state{.registers = expected, .failure = {}};
  if (fault) {
    state.failure = {.fault = Fault::memory_read,
                     .pc = entry,
                     .opcode = static_cast<Byte>(isa::Opcode::brset_extended),
                     .address = data,
                     .value = ignored_bits,
                     .access_size = 1,
                     .read_mask = mask};
  }
  const auto result = core.run(1);
  require(result.retired == static_cast<unsigned>(!fault) && result.fault == state.failure.fault &&
              core.state() == state &&
              core.progress().estimated_cycles == before + (fault ? 0U : branch_cycles),
          "Dynamic masked reads preserve independent branch PC, all registers/CCR, time and exact "
          "fault metadata");
  require(bus.calls == static_cast<unsigned>(mode >= 2) &&
              (mode < 2 || (bus.seen.address == data && bus.seen.mask == mask)),
          "Unknown dynamic bits use exactly one original masked callback, while mapped known bits "
          "stay direct");
}

void masked_reads() {
  constexpr Byte mask = 0x80;
  auto code = isa::program(
      isa::opcode<isa::Opcode::brset_extended>(),
      std::array<Byte, 4>{static_cast<Byte>(data >> byte_bits), static_cast<Byte>(data), mask, 1},
      isa::nop,
      isa::opcode<isa::Opcode::bra_relative>(),
      std::array<Byte, 1>{backedge});
  const auto image = prepare(code);
  const auto original = code;
  Masked bus;
  Byte value = 0;
  Byte known = 1;
  const std::array grants{Ram{.first = entry, .bytes = code, .writable = false},
                          Ram{.first = data, .bytes = {&value, 1}, .known = {&known, 1}}};
  Io io{.owner = &bus, .read_masked = Masked::read};
  Core core(Memory::mapped(grants), &io);
  core.install(image, 1);
  for (unsigned flags = 0; flags < 256; ++flags) {
    for (unsigned mode = 0; mode < 5; ++mode) {
      value = static_cast<Byte>(mode % 2 != 0 ? mask : 0);
      masked_prefix(core, bus, known, {.mode = mode, .flags = static_cast<Byte>(flags)});
      require(value == (mode % 2 != 0 ? mask : 0) && known == (mode < 2 ? 1 : 0) &&
                  code == original,
              "Dynamic masked reads never modify their RAM or original code");
    }
  }
}

}

int main() try {
  const auto image = prepare();
  admission(image);
  remaps(image);
  wrapped_word();
  masked_reads();
  std::cout << "Verified 1280 dynamic word remaps,256 wrapping word loads,1280 masked prefixes and "
               "strict source admission.\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
