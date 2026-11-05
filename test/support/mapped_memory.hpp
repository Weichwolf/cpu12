// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "isa.hpp"

#include <array>
#include <stdexcept>

namespace cpu12::test::mapped {
inline constexpr Address first = 0x2000, second = 0x7000;
inline constexpr Registers seed{.pc = 0x8000, .d = 0x1234, .x = first, .sp = 0x9000, .ccr = 0xff};

inline void require(const bool condition, const char *message) {
  if (!condition) { throw std::runtime_error(message); }
}

template <isa::Opcode Opcode> constexpr auto extended(const Address address) {
  constexpr unsigned byte_bits = 8;
  return isa::program(
      isa::opcode<Opcode>(),
      std::array<Byte, 2>{static_cast<Byte>(address >> byte_bits), static_cast<Byte>(address)});
}

struct Storage {
  std::array<Byte, 16> low{}, high{}, low_known{}, high_known{};

  [[nodiscard]] std::array<Ram, 3> grants() {
    return {{{.first = first, .bytes = low, .known = low_known},
             {.first = second, .bytes = high, .known = high_known},
             {}}};
  }
};

struct Owner {
  unsigned reads = 0, writes = 0, width = 0;
  Address address = 0;
  Core *core = nullptr;
  bool refused_remap = false;

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    value = 0;
    auto &self = *static_cast<Owner *>(owner);
    ++self.reads;
    self.address = address;
    self.width = 1;
    return false;
  }

  static bool word(void *owner, const Address address, Address &value) noexcept {
    value = 0;
    auto &self = *static_cast<Owner *>(owner);
    ++self.reads;
    self.address = address;
    self.width = 2;
    return false;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    auto &self = *static_cast<Owner *>(owner);
    ++self.writes;
    self.address = access.address;
    if (self.core != nullptr) {
      try {
        self.core->set_memory(Memory::external());
      } catch (const std::logic_error &) { self.refused_remap = true; }
    }
    return false;
  }

  [[nodiscard]] Io io() {
    return {.owner = this, .read = read, .write = write, .read_word = word, .fetch = read};
  }
};

void mapping_cases();
}
