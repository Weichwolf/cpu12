// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "jit/block.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace cpu12::detail {
[[nodiscard]] bool private_frame(const Memory &memory, Address stack);
[[nodiscard]] bool private_return_frame(const Memory &memory, Address stack);

struct PrefixLookup {
  const void *owner;
  std::optional<std::uint64_t> (*find)(const void *, Address);
};

enum class PrefixAccess : std::uint8_t { ordinary, guarded, boundary, terminal };

[[nodiscard]] std::map<Address, std::uint64_t>
foreground_prefixes(std::span<const BlockInstruction> instructions,
                    const Memory &memory,
                    std::span<const PrefixAccess> access = {});

class GuardedPrefixes {
public:
  GuardedPrefixes() = default;
  GuardedPrefixes(std::span<const BlockInstruction> instructions,
                  const Memory &memory,
                  std::span<const LocalAccess> local = {});
  [[nodiscard]] std::uint64_t prefix(Address pc, const Registers &registers) const;

private:
  struct Range {
    unsigned first, last;
    const Byte *known;
  };

  struct Guard {
    IndexBase base;
    Address offset;
    unsigned access;
    auto operator<=>(const Guard &) const = default;
  };

  struct Entry {
    std::uint64_t cycles;
    std::size_t group;
  };

  std::array<std::vector<Range>, 6> ranges_;
  std::vector<std::vector<Guard>> groups_;
  std::map<Address, Entry> entries_;
  PrefixAccess store_policy_ = PrefixAccess::guarded;
  [[nodiscard]] std::optional<Guard> guard(const BlockInstruction &operation) const;
  void bind_memory(const Memory &memory);
  void bind_local(std::span<const LocalAccess> local);
  void prepare(std::span<const BlockInstruction> instructions, const Memory &memory);
  void bind(std::span<const BlockInstruction> instructions,
            const std::map<Address, std::uint64_t> &prefixes,
            std::span<const std::optional<Guard>> guards,
            std::span<const PrefixAccess> policies);
};

[[nodiscard]] std::uint64_t interrupt_entry_prefix(const State &state,
                                                   const Memory &memory,
                                                   PrefixLookup foreground,
                                                   std::span<const InterruptRequest> interrupts);

[[nodiscard]] std::uint64_t
interrupt_entry_prefix(const State &state,
                       const Memory &memory,
                       const std::map<Address, std::uint64_t> &foreground,
                       std::span<const InterruptRequest> interrupts);
}
