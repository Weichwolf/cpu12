// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "multiply_divide_model.hpp"
#include "preparation_math.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>

namespace cpu12::test::prepared_math {
namespace {
struct Probe {
  std::uint64_t calls = 0;
  Address last = 0;

  void hit(const Address at) noexcept {
    ++calls;
    last = at;
  }

  static bool read(void *owner, const Address at, Byte &value) noexcept {
    static_cast<Probe *>(owner)->hit(at);
    value = 0;
    return false;
  }

  static bool write(void *owner, const ByteWrite access) noexcept {
    static_cast<Probe *>(owner)->hit(access.address);
    return false;
  }

  static bool read_word(void *owner, const Address at, Address &value) noexcept {
    static_cast<Probe *>(owner)->hit(at);
    value = 0;
    return false;
  }

  static bool write_word(void *owner, const WordWrite access) noexcept {
    static_cast<Probe *>(owner)->hit(access.address);
    return false;
  }

  Io io() {
    return {.owner = this,
            .read = read,
            .write = write,
            .read_word = read_word,
            .write_word = write_word};
  }
};

Count qualify(const Operation operation) {
  const Source source;
  const auto image = compile(source);
  auto code = source.bytes;
  auto second = code;
  Probe probe;
  auto io = probe.io();
  Core installed(Memory::window({.first = first, .bytes = code, .writable = false}), &io);
  Core graph(Memory::window({.first = first, .bytes = second, .writable = false}), &io);
  installed.install(image, variant);
  const auto report = graph.prepare(source.entries);
  require(report.failure.fault == Fault::none && report.unresolved_entries == 0 &&
              report.prepared_entries == operations.size() * 2 && report.native_bytes != 0 &&
              probe.calls == 0,
          "All math operations prepare before execution without touching registers, I/O or time");
  auto before = math::seed;
  const auto index =
      static_cast<std::size_t>(std::ranges::find(operations, operation) - operations.begin());
  before.pc = source.entries[index];
  Count count;
  for (Core *core : {&installed, &graph}) {
    const auto compiled = core->statistics().compiled_entries;
    domains(*core, {.operation = operation, .before = before}, count);
    prefixes(*core, {.operation = operation, .before = before});
    require(core->statistics().compiled_entries == compiled && probe.calls == 0,
            "Prepared math executes no ISA, memory or first-use translation helpers");
  }
  require(code == source.bytes && second == source.bytes,
          "Prepared math preserves every executable byte");
  wrapped(operation);
  return count;
}
}
}

int main(const int argc, const char *const *const argv) {
  try {
    using namespace cpu12::test::prepared_math;
    require(argc == 2, "Choose one independent math domain");
    const std::string_view selected(argv[1]);
    if (selected == "chain") {
      chain();
      return 0;
    }
    const auto *const found = std::ranges::find(names, selected);
    require(found != names.end(), "Unknown math domain");
    const auto index = static_cast<std::size_t>(found - names.begin());
    const auto count = qualify(operations[index]);
    std::cout
        << "Verified " << count.states << " independent prepared " << selected << " states; "
        << count.policy
        << " include explicit indeterminate-output policy, not a silicon-equivalence claim.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
