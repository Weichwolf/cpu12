// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/block.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace cpu12::detail::llvm19 {
struct ProgramMemory {
  Memory source, bindings;
};

enum class ArtifactOutput : std::uint8_t { none, native };

struct ProgramOptions {
  ProgramMemory memory;
  std::string prefix = "cpu12_region_";
  std::span<const std::byte> bindings;
  ArtifactOutput output = ArtifactOutput::none;
};

class Program {
public:
  Program(std::span<const BlockInstruction> instructions, const Memory &memory);
  Program(std::span<const BlockInstruction> instructions, ProgramOptions options);
  ~Program();
  Program(const Program &) = delete;
  Program &operator=(const Program &) = delete;
  [[nodiscard]] const void *entry(Address address) const;
  [[nodiscard]] const void *find(Address address) const;
  [[nodiscard]] const Instruction *instruction(Address address) const noexcept;
  [[nodiscard]] std::string_view symbol(Address address) const;
  [[nodiscard]] std::span<const std::byte> bitcode() const noexcept;
  [[nodiscard]] std::span<const std::byte> object() const noexcept;
  // Generated functions contain no captured RAM pointers. Reuse requires the
  // complete transitive native code and any lowered memory topology.
  [[nodiscard]] const void *find(std::span<const BlockInstruction> region,
                                 const Memory &memory) const;
  [[nodiscard]] std::uint64_t native_bytes() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
}
