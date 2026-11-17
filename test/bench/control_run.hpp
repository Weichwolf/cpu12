// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "control.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "machine.hpp"
#include "measurement.hpp"
#include "program.hpp"
#include "reference.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>

namespace cpu12::bench {
class ControlRun {
public:
  explicit ControlRun(const bool copying)
      : copying_(copying), machine_(copying ? copy_program() : control_program()) {
    for (unsigned i = 0; i < source_.size(); ++i) {
      constexpr unsigned multiplier = 0x9e37;
      constexpr unsigned salt = 0x123;
      source_[i] = static_cast<std::uint16_t>(
          (i * multiplier) ^
          std::rotl(static_cast<std::uint16_t>(i + salt), static_cast<int>(i % 16)));
    }
  }

  Measurement execute(Counters &counters) {
    machine_.core.restore(machine_.initial);
    std::array<std::uint16_t, copy_words> destination{};
    const auto expected_checksum = copying_ ? copy_checksum(source_, destination, copy_passes)
                                            : control(transitions, initial_checksum);
    put(machine_.ram, checksum, copying_ ? 0 : initial_checksum);
    put(machine_.ram, passes, copying_ ? copy_passes : control_passes);
    put(machine_.ram, control_state, 3);
    for (unsigned i = 0; i < 8; ++i) {
      put(machine_.ram,
          static_cast<Address>(handler_table + (i * 2)),
          copying_ ? 0 : machine_.program.at("handler" + std::to_string(i)));
    }
    if (copying_) {
      for (unsigned i = 0; i < source_.size(); ++i) {
        put(machine_.ram, static_cast<Address>(copy_source + (i * 2)), source_[i]);
      }
    }
    auto expected = machine_.ram;
    put(expected, checksum, expected_checksum);
    put(expected, passes, 0);
    if (copying_) {
      for (unsigned i = 0; i < destination.size(); ++i) {
        put(expected, static_cast<Address>(copy_destination + (i * 2)), destination[i]);
      }
    }
    const Registers registers{
        .pc = machine_.program.at("end"),
        .d = 0,
        .x = copying_ ? static_cast<Address>(copy_source + (copy_words * 2)) : handler_table,
        .y = copying_ ? static_cast<Address>(copy_destination + (copy_words * 2)) : Address{},
        .sp = stack_top - frame_bytes,
        .ccr = 4};
    frame(expected, registers);
    machine_.core.set_registers({.pc = code_base, .sp = stack_top});
    counters.start();
    const auto result = machine_.core.run();
    auto measured = counters.stop();
    measured.retired = result.retired;
    const auto instructions = copying_ ? copy_instructions : control_instructions;
    const auto cycles = copying_ ? copy_cycles : control_cycles;
    require(result.fault == Fault::none && result.retired == instructions &&
                machine_.core.progress().estimated_cycles == cycles &&
                machine_.core.registers() == registers && machine_.ram == expected &&
                machine_.core.state().execution == ExecutionState::waiting,
            "Control/copy complete RAM, registers or instruction/base-cycle count mismatch");
    return measured;
  }

  Measurement portable(Counters &counters) const {
    std::array<std::uint16_t, copy_words> destination{};
    counters.start();
    const auto result = copying_ ? copy_checksum(source_, destination, copy_passes)
                                 : control(transitions, initial_checksum);
    const auto measured = counters.stop();
    const auto expected = copying_ ? copy_checksum(source_, destination, copy_passes)
                                   : control(transitions, initial_checksum);
    require(result == expected && (!copying_ || destination == source_),
            "Portable copy/control comparison changed its complete result");
    return measured;
  }

  [[nodiscard]] std::uint64_t native_bytes() const { return machine_.image.report().native_bytes; }

  [[nodiscard]] double preparation_seconds() const { return machine_.preparation; }

private:
  static void frame(std::span<Byte> ram, const Registers registers) {
    // WAI writes CCR, B, A, X, Y and the return PC at descending SP.
    ram[registers.sp] = registers.ccr;
    ram[registers.sp + 1U] = static_cast<Byte>(registers.d);
    ram[registers.sp + 2U] = static_cast<Byte>(registers.d >> byte_bits);
    put(ram, static_cast<Address>(registers.sp + 3), registers.x);
    put(ram, static_cast<Address>(registers.sp + 5), registers.y);
    put(ram, static_cast<Address>(registers.sp + 7), registers.pc);
  }

  static constexpr Address initial_checksum = 0xace0;
  static constexpr unsigned transitions = 1U << 20, frame_bytes = 9;
  static constexpr std::uint64_t copy_instructions = 12585985, copy_cycles = 33561093;
  static constexpr std::uint64_t control_instructions = 16777298, control_cycles = 50331863;
  bool copying_;
  Machine machine_;
  std::array<std::uint16_t, copy_words> source_{};
};
}
