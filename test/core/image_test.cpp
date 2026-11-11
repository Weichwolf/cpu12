// SPDX-License-Identifier: GPL-3.0-only
#include "code_image.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "image_model.hpp"
#include "image_support.hpp"

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <utility>

namespace cpu12::test::image {
namespace {
std::size_t prefixes(Core &core, Fixture &fixture) {
  const auto original = fixture.bytes;
  const auto compiled = core.statistics().compiled_entries;
  std::size_t count = 0;
  for (unsigned selection = 0; selection < 4; ++selection) {
    for (const auto ccr : flags) {
      for (const auto grant : grants) {
        fixture.bytes = original;
        auto before = initial;
        before.d = static_cast<Address>(before.d | selection);
        before.ccr = ccr;
        core.set_registers(before);
        const auto cycles = core.progress().estimated_cycles;
        const auto expected = predict(before, grant, fixture);
        auto memory = original;
        if (grant != 0) {
          memory[before.sp - 2] = static_cast<Byte>(table >> byte_bits);
          memory[before.sp - 1] = static_cast<Byte>(table);
        }
        const auto result = core.run(grant);
        require(result.retired == grant && result.fault == Fault::none &&
                    core.state() == State{.registers = expected.registers, .failure = {}} &&
                    core.progress() == Progress{.estimated_cycles = cycles + expected.cycles} &&
                    fixture.bytes == memory && core.statistics().compiled_entries == compiled,
                "Installed public execution must preserve independent state/progress/RAM prefixes "
                "without translation");
        ++count;
      }
    }
  }
  return count;
}

std::size_t owners() {
  Fixture first{Source(negative)};
  Fixture second{Source(0)};
  Fixture independent{Source(negative)};
  independent.bytes[table] = independent.bytes[table + 2];
  Core a(first.memory());
  Core b(second.memory());
  Core c(independent.memory());
  {
    const auto image = compile();
    const auto before = a.state();
    const auto progress = a.progress();
    const auto bytes = first.bytes;
    a.install(image, first_id);
    b.install(image, second_id);
    auto shared = image;
    const auto transferred = std::move(shared);
    c.install(transferred, first_id);
    const auto report = image.report();
    require(report.prepared_entries == declared_sites && report.unresolved_entries == 0 &&
                report.failure.fault == Fault::none && report.native_bytes > 0 &&
                a.state() == before && a.progress() == progress && first.bytes == bytes,
            "Owner-free compilation and cold installation must preserve actual owner state and "
            "own all source/descriptor/image lifetimes");
  }
  return prefixes(a, first) + prefixes(b, second) + prefixes(c, independent);
}
}
}

int main() {
  try {
    using namespace cpu12::test::image;
    const auto count = owners();
    image_contracts();
    image_events();
    image_deadlines();
    image_io();
    std::cout << "Verified " << count
              << " public installed prefixes, image ownership, "
                 "strict execution/remaps, prepared events and deadlines.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
