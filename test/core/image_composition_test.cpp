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
constexpr Address first = 0x400;
constexpr std::uint64_t a = 7, b = 9, alias = 11, wrong = 13;
constexpr Byte nzv = 0x0e;
constexpr Byte low_value = 0x7f, high_value = 0x22, wrong_value = 0x33, transfer_flags = 0x81;
constexpr Address ram_first = 0x900, irq_vector = 0xfff0, stack = 0xaff;
constexpr Address seed_d = 0x1234, seed_x = 0x5678, seed_y = 0x9abc, irq_d = 0x2234;
constexpr unsigned low_mask = 0xff, high_mask = 0xff00, high_word = 0x2200;
constexpr std::uint64_t unknown = 99;

void require(bool value, const char *message) {
  if (!value) { throw std::runtime_error(message); }
}

template <class Action> void rejects(Action action, std::string_view reason) {
  bool refused = false;
  try {
    action();
  } catch (const std::invalid_argument &error) {
    refused = std::string_view(error.what()).contains(reason);
  }
  require(refused, "Invalid prepared composition must fail with its specific contract");
}

struct Fixture {
  Address second;
  std::array<Byte, 5> left, right;
  std::array<Byte, 512> ram{};
  std::array<Byte, 512> known{};
  std::array<Byte, 2> vector{};
  std::array<Ram, 4> grants;

  explicit Fixture(Address destination)
      : second(destination),
        left{isa::opcode<isa::Opcode::ldab_immediate>()[0],
             low_value,
             isa::opcode<isa::Opcode::jmp_extended>()[0],
             static_cast<Byte>(destination >> 8U),
             static_cast<Byte>(destination)},
        right(isa::program(
            isa::opcode<isa::Opcode::ldaa_immediate>(),
            std::array{high_value},
            isa::opcode<isa::Opcode::jmp_extended>(),
            std::array<Byte, 2>{static_cast<Byte>(first >> 8U), static_cast<Byte>(first)})),
        vector{static_cast<Byte>(destination >> 8U), static_cast<Byte>(destination)},
        grants{Ram{.first = first, .bytes = left, .writable = false},
               Ram{.first = second, .bytes = right, .writable = false},
               Ram{.first = ram_first, .bytes = ram, .known = known},
               Ram{.first = irq_vector, .bytes = vector, .writable = false}} {}

  Memory memory() { return Memory::mapped(grants); }
};

NativeImage
prepare(Address second, bool complete = true, MemoryTopology topology = MemoryTopology::dynamic) {
  Fixture source(second);
  auto incorrect = source.right;
  incorrect[1] = wrong_value;
  const std::array l{CodeSpan{.address = first, .words = source.left}};
  const std::array r{CodeSpan{.address = second, .words = source.right}};
  const std::array bad{CodeSpan{.address = second, .words = incorrect}};
  const std::array<Address, 2> roots{first, first + 2};
  const std::array<Address, 2> targets{second, static_cast<Address>(second + 2)};
  const std::array variants{CodeVariant{.id = a, .code = l, .entries = roots},
                            CodeVariant{.id = b, .code = r, .entries = targets},
                            CodeVariant{.id = alias, .code = r, .entries = targets},
                            CodeVariant{.id = wrong, .code = bad, .entries = targets}};
  std::vector<CodeTransition> edges;
  for (const auto target : {b, alias, wrong}) {
    for (const auto pc : targets) {
      if (complete || pc == second) { edges.push_back({.from = a, .to = target, .entry = pc}); }
    }
    for (const auto pc : roots) { edges.push_back({.from = target, .to = a, .entry = pc}); }
  }
  const std::array events{InterruptRequest{.vector = irq_vector}};
  const std::array layout{RegionLayout{.first = first, .size = 5, .writable = false},
                          RegionLayout{.first = second, .size = 5, .writable = false},
                          RegionLayout{.first = ram_first, .size = 512, .tracked = true},
                          RegionLayout{.first = irq_vector, .size = 2, .writable = false}};
  std::array<VariantLayout, 4> layouts{};
  for (std::size_t i = 0; i < variants.size(); ++i) {
    layouts[i] = {.id = variants[i].id, .memory = layout};
  }
  return Core::compile({.variants = variants},
                       {.memory = topology == MemoryTopology::fixed
                                      ? std::span<const VariantLayout>(layouts)
                                      : std::span<const VariantLayout>{},
                        .interrupts = events,
                        .transitions = edges,
                        .callbacks = {},
                        .bindings = {},
                        .topology = topology});
}

Registers seed(Byte flags) {
  return {.pc = first, .d = seed_d, .x = seed_x, .y = seed_y, .sp = stack, .ccr = flags};
}

Registers predict(Address second, Registers state, unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    switch (i % 4) {
      case 0:
        state.d = static_cast<Address>((state.d & high_mask) | low_value);
        state.ccr = static_cast<Byte>(state.ccr & ~nzv);
        state.pc = first + 2;
        break;
      case 1: state.pc = second; break;
      case 2:
        state.d = static_cast<Address>((state.d & low_mask) | high_word);
        state.ccr = static_cast<Byte>(state.ccr & ~nzv);
        state.pc = static_cast<Address>(second + 2);
        break;
      case 3: state.pc = first; break;
      default: throw std::logic_error("Invalid authored instruction phase");
    }
  }
  return state;
}

std::uint64_t cycles(unsigned count) {
  return ((count / 2U) * 4U) + (count % 2U);
}

void prefixes(const NativeImage &image, Address second) {
  constexpr std::array group{a, b};
  constexpr std::array reverse{b, a};
  for (unsigned flags = 0; flags < 256; ++flags) {
    const auto &selected = (flags & 1U) == 0 ? group : reverse;
    for (unsigned kind = 0; kind < 2; ++kind) {
      const unsigned bound = kind == 0 ? 18U : 26U;
      for (unsigned limit = 0; limit < bound; ++limit) {
        Fixture f(second);
        Core core(f.memory());
        core.install(image, selected);
        const auto before = seed(static_cast<Byte>(flags));
        core.set_registers(before);
        unsigned count = limit;
        if (kind != 0) {
          count = 0;
          while (cycles(count) < limit) { ++count; }
        }
        const auto result = kind == 0 ? core.run(limit) : core.run_until(limit);
        require(result.fault == Fault::none && result.retired == count &&
                    core.state() ==
                        State{.registers = predict(second, before, count), .failure = {}} &&
                    core.progress() == Progress{.estimated_cycles = cycles(count)} &&
                    core.statistics().compiled_entries == 0 &&
                    std::ranges::all_of(f.ram, [](Byte v) { return v == 0; }) &&
                    std::ranges::all_of(f.known, [](Byte v) { return v == 0; }),
                "Mounted native loops preserve independent full-CCR/quota/deadline state, cycles "
                "and every RAM/knownness byte");
      }
    }
  }
}

void contracts(const NativeImage &image, Address second) {
  Fixture f(second);
  Core core(f.memory());
  core.install(image, a);
  core.set_registers(seed(transfer_flags));
  const auto state = core.state();
  const auto time = core.progress();
  rejects([&] { core.install(image, std::span<const std::uint64_t>{}); },
          "requires prepared domains");
  rejects([&] { core.install(image, std::array{a, a}); }, "Duplicate");
  rejects([&] { core.install(image, std::array{a, unknown}); }, "Unknown");
  rejects([&] { core.install(image, std::array{a, wrong}); }, "executable source does not match");
  rejects([&] { core.install(image, std::array{a, b, alias}); }, "overlap executable bytes");
  require(core.state() == state && core.progress() == time,
          "Every failed composition leaves the original owner unchanged");
  const auto prefix = core.run(2);
  require(prefix.retired == 2 && prefix.fault == Fault::none,
          "Original scalar jump completes before a pending prepared target");
  const auto pending = core.state();
  const auto progress = core.progress();
  core.set_memory(f.memory());
  rejects([&] { core.install(image, std::array{a, wrong}); }, "executable source does not match");
  require(core.run(8).retired == 0 && core.state() == pending && core.progress() == progress,
          "Rejected composite target preserves pending jump and every completed effect");
  core.install(image, std::array{a, b});
  require(core.state() == pending && core.progress() == progress,
          "Mounting an actual matching prepared target never replays the jump");
  require(
      core.run(6).retired == 6 && core.registers() == predict(second, seed(transfer_flags), 8) &&
          core.progress() == Progress{.estimated_cycles = cycles(8)} &&
          core.statistics().compiled_entries == 0,
      "Suspended scalar transfer resumes through mounted domains with the exact original suffix");
  const auto fixed = prepare(second, true, MemoryTopology::fixed);
  rejects([&] { core.install(fixed, std::array{a, b}); }, "require dynamic topology");
  const auto partial = prepare(second, false);
  rejects([&] { core.install(partial, std::array{a, b}); }, "every declared cross-domain entry");
}

void interrupts(const NativeImage &image, Address second) {
  Fixture f(second);
  Core core(f.memory());
  core.install(image, std::array{a, b});
  const auto before = seed(0);
  core.set_registers(before);
  const auto result = core.interrupt({.vector = irq_vector});
  auto expected = before;
  expected.pc = second;
  expected.sp = static_cast<Address>(before.sp - 9);
  expected.ccr = 0x10;
  require(result.accepted && result.fault == Fault::none && core.registers() == expected,
          "Original IRQ entry targets another mounted domain without suspending execution");
  const std::array<Byte, 9> frame{0, 0x34, 0x12, 0x56, 0x78, 0x9a, 0xbc, 0x04, 0};
  for (std::size_t i = 0; i < f.ram.size(); ++i) {
    const auto at = static_cast<std::size_t>(expected.sp - ram_first);
    const bool stored = i >= at && i - at < frame.size();
    require(f.ram[i] == (stored ? frame[i - at] : 0) && f.known[i] == (stored ? 1 : 0),
            "Every original IRQ stack byte and surrounding knownness is exact");
  }
  require(core.run(1).retired == 1 && core.registers().pc == second + 2 &&
              core.registers().d == irq_d && core.statistics().compiled_entries == 0,
          "The mounted original IRQ target executes once without installation or new code");
}

void retained_owners() {
  constexpr Address second = 0x480;
  Fixture left(second);
  Fixture right(second);
  Core first_owner(left.memory());
  Core second_owner(right.memory());
  {
    const auto image = prepare(second);
    first_owner.install(image, std::array{a, b});
    second_owner.install(image, std::array{a, alias});
  }
  const auto initial = seed(transfer_flags);
  first_owner.set_registers(initial);
  second_owner.set_registers(initial);
  constexpr unsigned count = 1024;
  require(first_owner.run(count).retired == count && second_owner.run(count).retired == count &&
              first_owner.registers() == predict(second, initial, count) &&
              first_owner.state() == second_owner.state() &&
              first_owner.progress() == Progress{.estimated_cycles = cycles(count)} &&
              first_owner.progress() == second_owner.progress() &&
              first_owner.statistics().compiled_entries == 0 &&
              second_owner.statistics().compiled_entries == 0,
          "Distinct mounted owners retain shared native code and independent merged pages after "
          "every public image owner is destroyed");
}

void returns() {
  constexpr Address second = 0x480;
  constexpr unsigned irq_cycles = 9;
  constexpr unsigned rti_cycles = 8;
  Fixture source(second);
  source.right[0] = isa::rti[0];
  const std::array caller{CodeSpan{.address = first, .words = source.left}};
  const std::array handler{CodeSpan{.address = second, .words = std::span(source.right).first(1)}};
  const std::array<Address, 2> roots{first, first + 2};
  const std::array targets{second};
  const std::array variants{CodeVariant{.id = a, .code = caller, .entries = roots},
                            CodeVariant{.id = b, .code = handler, .entries = targets}};
  const std::array edges{CodeTransition{.from = a, .to = b, .entry = second},
                         CodeTransition{.from = b, .to = a, .entry = first},
                         CodeTransition{.from = b, .to = a, .entry = first + 2}};
  const std::array events{InterruptRequest{.vector = irq_vector}};
  const auto image = Core::compile({.variants = variants},
                                   {.memory = {},
                                    .interrupts = events,
                                    .transitions = edges,
                                    .callbacks = {},
                                    .bindings = {},
                                    .topology = MemoryTopology::dynamic});
  for (unsigned flags = 0; flags < 256; ++flags) {
    constexpr unsigned mask_i = 0x10;
    if ((flags & mask_i) != 0) { continue; }
    Fixture f(second);
    f.right[0] = isa::rti[0];
    Core core(f.memory());
    core.install(image, (flags & 1U) == 0 ? std::array{a, b} : std::array{b, a});
    const auto before = seed(static_cast<Byte>(flags));
    core.set_registers(before);
    require(core.interrupt(events[0]).accepted &&
                core.progress() == Progress{.estimated_cycles = irq_cycles},
            "Original mounted IRQ entry consumes nine nominal cycles");
    const auto result = core.step();
    require(result.retired == 1 && result.fault == Fault::none &&
                core.state() == State{.registers = before, .failure = {}, .returned_frame = true} &&
                core.progress() == Progress{.estimated_cycles = irq_cycles + rti_cycles},
            "RTI in another mounted domain restores the original full frame, flags and cycles");
    require(core.run(1).retired == 1 && !core.state().returned_frame &&
                core.registers() == predict(second, before, 1) &&
                core.progress() == Progress{.estimated_cycles = irq_cycles + rti_cycles + 1} &&
                core.statistics().compiled_entries == 0,
            "The restored domain closes its exact return window on its first original instruction");
  }
}

struct RejectedWrite {
  const Core *core = nullptr;
  unsigned calls = 0;
  Registers observed;
  ByteWrite access;

  static bool write(void *owner, ByteWrite value) noexcept {
    auto &record = *static_cast<RejectedWrite *>(owner);
    ++record.calls;
    record.observed = record.core->registers();
    record.access = value;
    return false;
  }
};

void fault_prefixes() {
  constexpr Address second = 0x480;
  constexpr Address unmapped = 0x800;
  constexpr auto store = isa::program(isa::opcode<isa::Opcode::stab_extended>(),
                                      std::array<Byte, 2>{8, 0},
                                      std::array<Byte, 2>{0x20, 0xfe});
  Fixture source(second);
  source.right = store;
  const std::array left{CodeSpan{.address = first, .words = source.left}};
  const std::array right{CodeSpan{.address = second, .words = source.right}};
  const std::array<Address, 2> roots{first, first + 2};
  const std::array<Address, 2> targets{second, second + 3};
  const std::array variants{CodeVariant{.id = a, .code = left, .entries = roots},
                            CodeVariant{.id = b, .code = right, .entries = targets}};
  std::vector<CodeTransition> edges;
  edges.reserve(roots.size() + targets.size());
  for (const auto pc : roots) { edges.push_back({.from = b, .to = a, .entry = pc}); }
  for (const auto pc : targets) { edges.push_back({.from = a, .to = b, .entry = pc}); }
  const auto image = Core::compile({.variants = variants},
                                   {.memory = {},
                                    .interrupts = {},
                                    .transitions = edges,
                                    .callbacks = {},
                                    .bindings = {},
                                    .topology = MemoryTopology::dynamic});
  for (unsigned flags = 0; flags < 256; ++flags) {
    Fixture f(second);
    f.right = store;
    RejectedWrite record;
    Io io{.owner = &record, .write = RejectedWrite::write};
    Core core(f.memory(), &io);
    record.core = &core;
    core.install(image, std::array{a, b});
    const auto before = seed(static_cast<Byte>(flags));
    core.set_registers(before);
    const auto result = core.run(8);
    const auto expected = predict(second, before, 2);
    require(result.fault == Fault::memory_write && result.retired == 2 && result.pc == second &&
                result.address == unmapped && result.value == low_value &&
                result.access_size == 1 && core.registers() == expected &&
                core.progress() == Progress{.estimated_cycles = cycles(2)} && record.calls == 1 &&
                record.observed == expected && record.access.address == unmapped &&
                record.access.value == low_value &&
                std::ranges::all_of(f.ram, [](Byte value) { return value == 0; }) &&
                std::ranges::all_of(f.known, [](Byte value) { return value == 0; }) &&
                core.statistics().compiled_entries == 0,
            "A real I/O fault after a native domain crossing preserves every original prefix "
            "effect, published callback state and fault field");
  }
}

}

int main() try {
  fault_prefixes();
  retained_owners();
  returns();
  for (const Address second : {Address{0x480}, Address{0x600}}) {
    const auto image = prepare(second);
    prefixes(image, second);
    contracts(image, second);
    interrupts(image, second);
  }
  std::cout << "Verified 22528 independent mounted CCR/quota/deadline prefixes, same/different "
               "native pages, strict compositions, pending remaps, retained owners, original IRQ "
               "frames and128 cross-domain RTI returns.\n";
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
