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
#include <optional>
#include <span>
#include <stdexcept>

namespace {
using namespace cpu12;
using Opcode = isa::Opcode;
constexpr Address entry = 0x200, handler = 0x300, load = 0x400, halted = 0x500;
constexpr Address input = 0x1000, stack = 0x9000, vector = 0xfff0;
constexpr unsigned byte_bits = 8;
constexpr Byte sample = 0x5a;
constexpr std::uint64_t variant = 1;
constexpr Byte low_value = 0xa5;
constexpr Address loaded_value = (Address{sample} << byte_bits) | low_value;
constexpr Registers initial{
    .pc = entry, .d = 0x12a5, .x = 0x3456, .y = 0x789a, .sp = stack, .ccr = 0x10};
constexpr auto malformed_execution = static_cast<ExecutionState>(0xff);
constexpr std::array<Byte, 2> spin{0x20, 0xfe};
constexpr auto main_code = isa::program(
    isa::opcode<Opcode::andcc_immediate>(), std::array<Byte, 1>{0xef}, isa::nop, isa::wai, spin);
constexpr auto handler_code =
    isa::program(isa::opcode<Opcode::ldaa_immediate>(), std::array<Byte, 1>{sample}, isa::rti);
constexpr auto load_code =
    isa::program(isa::opcode<Opcode::ldaa_extended>(), std::array<Byte, 2>{0x10, 0}, spin);
constexpr auto stop_code = isa::program(isa::stop, spin);
const std::array spans{CodeSpan{.address = entry, .words = main_code},
                       CodeSpan{.address = handler, .words = handler_code},
                       CodeSpan{.address = load, .words = load_code},
                       CodeSpan{.address = halted, .words = stop_code}};
constexpr std::array entries{entry, handler, load, halted};
constexpr std::array layout{
    RegionLayout{.first = entry, .size = main_code.size(), .writable = false},
    RegionLayout{.first = handler, .size = handler_code.size(), .writable = false},
    RegionLayout{.first = load, .size = load_code.size(), .writable = false},
    RegionLayout{.first = halted, .size = stop_code.size(), .writable = false}};

void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

struct Device {
  std::array<Byte, address_bytes> ram{};
  std::array<Ram, spans.size()> grants{};
  Io io{.owner = this,
        .read = read,
        .write = write,
        .read_word = read_word,
        .write_word = write_word};
  Core core{Memory::external(), &io};
  std::optional<Snapshot> probe;
  unsigned reads = 0, writes = 0;
  bool fail = false, save_rejected = false, restore_rejected = false;

  explicit Device(const NativeImage &image) {
    for (std::size_t index = 0; index < spans.size(); ++index) {
      const auto code = spans[index];
      std::ranges::copy(code.words, ram.begin() + code.address);
      grants[index] = {.first = code.address,
                       .bytes = std::span(ram).subspan(code.address, code.words.size()),
                       .writable = false};
    }
    ram[input] = sample;
    ram[vector] = static_cast<Byte>(handler >> byte_bits);
    ram[vector + 1] = static_cast<Byte>(handler);
    core.set_memory(Memory::mapped(grants));
    core.install(image, variant);
    core.set_registers(initial);
  }

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    auto &self = *static_cast<Device *>(owner);
    ++self.reads;
    if (self.probe) {
      try {
        static_cast<void>(self.core.save());
      } catch (const std::logic_error &) { self.save_rejected = true; }
      try {
        self.core.restore(*self.probe);
      } catch (const std::logic_error &) { self.restore_rejected = true; }
    }
    value = self.ram[address];
    return address != input || !self.fail;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &self = *static_cast<Device *>(owner);
    ++self.writes;
    self.ram[access.address] = access.value;
    return true;
  }

  static bool read_word(void *owner, const Address address, Address &value) noexcept {
    Byte high = 0;
    Byte low = 0;
    if (!read(owner, address, high) || !read(owner, static_cast<Address>(address + 1U), low)) {
      return false;
    }
    value = static_cast<Address>((unsigned{high} << byte_bits) | low);
    return true;
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    return write(owner,
                 {.address = access.address,
                  .value = static_cast<Byte>(access.value >> byte_bits)}) &&
           write(owner,
                 {.address = static_cast<Address>(access.address + 1U),
                  .value = static_cast<Byte>(access.value)});
  }
};

void shadow_and_memory(const NativeImage &image) {
  Device device(image);
  auto &core = device.core;
  require(core.run(1).retired == 1 && core.state().irq_delay == 1 &&
              !core.interrupt_ready(InterruptKind::maskable),
          "ANDCC leaves its real interrupt shadow");
  const auto saved = core.save();
  require(saved.progress.estimated_cycles == 1 && saved.state.registers.pc == entry + 2,
          "Checkpoint includes completed nominal time and the real next instruction");
  require(core.run(1).retired == 1 && core.interrupt_ready(InterruptKind::maskable),
          "NOP consumes the interrupt shadow");
  device.ram[input] ^= sample;
  const auto memory = device.ram;
  const auto reads = device.reads;
  const auto writes = device.writes;
  core.restore(saved);
  require(core.save() == saved && device.ram == memory && device.reads == reads &&
              device.writes == writes,
          "Restore changes only processor state, without memory, I/O or code preparation");
  require(core.run_until(2).retired == 1 && core.progress().estimated_cycles == 2 &&
              core.registers().pc == entry + 3 && core.state().irq_delay == 0,
          "A restored cycle deadline resumes the same prepared continuation");
}

void independent_owner(const NativeImage &image) {
  Device first(image);
  Device second(image);
  constexpr Byte other_sample = 0x80;
  constexpr Address expected = (Address{other_sample} << byte_bits) | low_value;
  first.core.set_registers({.pc = load, .d = low_value, .sp = stack});
  const auto saved = first.core.save();
  second.ram[input] = other_sample;
  second.core.restore(saved);
  require(
      second.core.run(1).retired == 1 && second.core.registers().d == expected &&
          second.core.progress().estimated_cycles == 3 && second.reads == 1 && first.reads == 0 &&
          first.core.save() == saved && second.ram[input] == other_sample,
      "A transferred checkpoint retains the receiving core's memory, callbacks and native code");
}

void frames(const NativeImage &image) {
  Device device(image);
  auto &core = device.core;
  require(core.run(1).retired == 1 && core.run(1).retired == 1 &&
              core.run(1).state == ExecutionState::waiting && core.registers().sp == stack - 9,
          "WAI creates its real nine-byte frame");
  const auto waiting = core.save();
  const auto memory = device.ram;
  const auto enter = [&] {
    const auto before = core.progress().estimated_cycles;
    require(core.interrupt({.vector = vector}).accepted && core.registers().pc == handler &&
                core.progress().estimated_cycles == before + 6,
            "Restored WAI/RTI reuses the frame in six cycles");
    require(core.run(2).retired == 2 && core.state().returned_frame && core.registers().sp == stack,
            "The actual handler RTI opens the returned-frame window");
  };
  enter();
  const auto returned = core.save();
  core.set_registers({.pc = load});
  core.restore(returned);
  require(core.save() == returned, "Restore retains the pending RTI arbitration window");
  enter();
  device.ram = memory;
  core.restore(waiting);
  require(core.save() == waiting && core.run(1).retired == 0,
          "A saved WAI remains asleep until a real interrupt");
  enter();
}

void stop(const NativeImage &image) {
  Device device(image);
  device.core.set_registers({.pc = halted, .sp = stack, .ccr = 0x40});
  require(device.core.run(1).state == ExecutionState::stopped, "STOP must really stop");
  const auto saved = device.core.save();
  device.core.set_registers({.pc = entry});
  device.core.restore(saved);
  require(device.core.run(1).retired == 0 && device.core.save() == saved,
          "Restore preserves STOP rather than resetting or executing it");
  require(device.core.resume_from_stop().accepted && device.core.registers().pc == halted + 2 &&
              device.core.progress() == saved.progress &&
              device.core.state().execution == ExecutionState::running,
          "The prepared masked-XIRQ wake preserves restored time without vector entry");
}

void faults_and_reentrancy(const NativeImage &image) {
  Device device(image);
  auto &core = device.core;
  core.set_registers({.pc = load, .d = low_value, .sp = stack});
  const auto healthy = core.save();
  device.fail = true;
  const auto failure = core.run(1);
  require(failure.fault == Fault::memory_read && failure.address == input && failure.retired == 0,
          "Checkpoint fixture must latch a genuine external read fault");
  const auto failed = core.save();
  device.fail = false;
  core.restore(healthy);
  device.probe = healthy;
  require(core.run(1).retired == 1 && core.registers().d == loaded_value && device.save_rejected &&
              device.restore_rejected,
          "Save/restore reject active callbacks without corrupting the resumed native instruction");
  device.probe.reset();
  const auto reads = device.reads;
  core.restore(failed);
  require(core.save() == failed && core.run(1) == failure && device.reads == reads,
          "Restored faults retain their context and latch without retrying external I/O");
  auto invalid = healthy;
  invalid.state.execution = malformed_execution;
  bool rejected = false;
  try {
    core.restore(invalid);
  } catch (const std::invalid_argument &) { rejected = true; }
  require(rejected && core.save() == failed && device.reads == reads,
          "Malformed snapshots fail before changing the processor");
}
}

int main() try {
  const std::array variants{CodeVariant{.id = variant, .code = spans, .entries = entries}};
  const std::array layouts{VariantLayout{.id = variant, .memory = layout}};
  const std::array requests{InterruptRequest{.vector = vector}};
  const auto image = Core::compile({.variants = variants},
                                   {.memory = layouts,
                                    .interrupts = requests,
                                    .transitions = {},
                                    .callbacks = {},
                                    .bindings = {}});
  shadow_and_memory(image);
  independent_owner(image);
  frames(image);
  stop(image);
  faults_and_reentrancy(image);
} catch (const std::exception &error) {

  std::cerr << error.what() << '\n';
  return 1;
}
