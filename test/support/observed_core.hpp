// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"

#include <cstdint>
#include <limits>
#include <span>

namespace cpu12::test {
struct Observations {
  std::uint64_t native_instructions = 0, compiled_entries = 0;
  std::uint64_t read_callbacks = 0, write_callbacks = 0, fetch_callbacks = 0;
};

// Observe actual owner calls, including refusals, without instrumentation in the core.
struct ObservedIo {
  explicit ObservedIo(const Io *const io) : input_(io != nullptr ? *io : Io{}) {
    output_ = {.owner = this,
               .read = input_.read != nullptr ? read : nullptr,
               .write = input_.write != nullptr ? write : nullptr,
               .read_word = input_.read_word != nullptr ? read_word : nullptr,
               .write_word = input_.write_word != nullptr ? write_word : nullptr,
               .fetch = input_.fetch != nullptr ? fetch : nullptr,
               .read_masked = input_.read_masked != nullptr ? masked : nullptr};
  }

  Io output_;
  Observations observed_;

  Io input_;

  static bool read(void *const owner, const Address address, Byte &value) noexcept {
    auto &self = *static_cast<ObservedIo *>(owner);
    ++self.observed_.read_callbacks;
    return self.input_.read(self.input_.owner, address, value);
  }

  static bool write(void *const owner, const ByteWrite access) noexcept {
    auto &self = *static_cast<ObservedIo *>(owner);
    ++self.observed_.write_callbacks;
    return self.input_.write(self.input_.owner, access);
  }

  static bool read_word(void *const owner, const Address address, Address &value) noexcept {
    auto &self = *static_cast<ObservedIo *>(owner);
    ++self.observed_.read_callbacks;
    return self.input_.read_word(self.input_.owner, address, value);
  }

  static bool write_word(void *const owner, const WordWrite access) noexcept {
    auto &self = *static_cast<ObservedIo *>(owner);
    ++self.observed_.write_callbacks;
    return self.input_.write_word(self.input_.owner, access);
  }

  static bool fetch(void *const owner, const Address address, Byte &value) noexcept {
    auto &self = *static_cast<ObservedIo *>(owner);
    ++self.observed_.fetch_callbacks;
    return self.input_.fetch(self.input_.owner, address, value);
  }

  static bool masked(void *const owner, const MaskedByteRead access, Byte &value) noexcept {
    auto &self = *static_cast<ObservedIo *>(owner);
    ++self.observed_.read_callbacks;
    return self.input_.read_masked(self.input_.owner, access, value);
  }
};

// Correctness fixtures count returned retirement outside the JIT. Benchmarks use Core directly.
class ObservedCore : private ObservedIo, public cpu12::Core {
public:
  explicit ObservedCore(const Memory memory = Memory::external(), Io *const io = nullptr)
      : ObservedIo(io), cpu12::Core(memory, &output_) {}

  [[nodiscard]] RunResult run_instruction(const std::span<const Byte> bytes) {
    return observe(cpu12::Core::run_instruction(bytes));
  }

  [[nodiscard]] RunResult step() { return observe(cpu12::Core::step()); }

  [[nodiscard]] RunResult
  run(const std::uint32_t budget = std::numeric_limits<std::uint32_t>::max()) {
    return observe(cpu12::Core::run(budget));
  }

  [[nodiscard]] Observations statistics() const noexcept {
    auto result = observed_;
    result.compiled_entries = cpu12::Core::statistics().compiled_entries;
    return result;
  }

private:
  RunResult observe(const RunResult result) noexcept {
    observed_.native_instructions += result.retired;
    return result;
  }
};
}
