// SPDX-License-Identifier: GPL-3.0-only
#include "bits_model.hpp"
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "indexed_model.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>

namespace {
using namespace cpu12;
using namespace cpu12::test::bits;
namespace indexed = cpu12::test::indexed;
constexpr std::array<unsigned, 8> values{0, 1, 0x7f, 0x80, 0xfe, 0xff, 0x55, 0xaa};
constexpr std::array<unsigned, 4> flags{0, 0xff, 0xa5, 0x5a};

void one(Core &core, const std::span<Byte> ram, const Code &code, const Point point) {
  ram[point.address] = static_cast<Byte>(point.data);
  const auto left = static_cast<std::uint16_t>(point.address + 0xffffU);
  const auto right = static_cast<std::uint16_t>(point.address + 1U);
  constexpr Byte left_guard = 0xa5;
  constexpr Byte right_guard = 0x5a;
  ram[left] = left_guard;
  ram[right] = right_guard;
  retired(core, code, point);
  const auto wanted = point.operation.branch
                          ? point.data
                          : modified(point.operation, {.data = point.data, .mask = point.mask});
  require(ram[point.address] == wanted && ram[left] == left_guard && ram[right] == right_guard,
          "Bit branches never write, and bit modifications affect only the selected byte");
}

struct Layout {
  std::uint16_t address;
  bool direct, partial;
};

void complete_domain(const Layout layout) {
  std::array<Byte, address_bytes> ram{};
  constexpr std::uint16_t first = 0x80;
  constexpr unsigned size = 0x80;
  Core core(layout.partial
                ? Memory::window({.first = first, .bytes = std::span(ram).subspan(first, size)})
                : Memory::unified(ram));
  const auto start = checked;
  for (const auto operation : operations) {
    for (unsigned mask = 0; mask <= byte_mask; ++mask) {
      for (unsigned data = 0; data <= byte_mask; ++data) {
        for (const auto ccr : flags) {
          auto before = seed;
          before.ccr = static_cast<Byte>(ccr);
          const Point point{.operation = operation,
                            .before = before,
                            .updated = before,
                            .address = layout.address,
                            .data = data,
                            .mask = mask};
          one(core, ram, absolute(point, layout.direct), point);
        }
      }
    }
  }
  require(
      core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0 &&
          core.statistics().native_instructions == checked - start,
      "Complete mask/value domains use direct native full/partial RAM without bus or ISA helpers");
}

void flag_domain() {
  std::array<Byte, address_bytes> ram{};
  Core core(Memory::unified(ram));
  for (const auto operation : operations) {
    for (unsigned mask = 0; mask <= byte_mask; ++mask) {
      for (unsigned ccr = 0; ccr <= byte_mask; ++ccr) {
        auto before = seed;
        before.ccr = static_cast<Byte>(ccr);
        for (const auto data : values) {
          const Point point{.operation = operation,
                            .before = before,
                            .updated = before,
                            .address = 0,
                            .data = data,
                            .mask = mask,
                            .displacement = 0};
          one(core, ram, absolute(point, true), point);
        }
      }
    }
  }
}

Code indexed_code(const indexed::Form form, const Point point) {
  const auto prefix = indexed::encode(form, point.operation.indexed);
  Code code;
  for (const auto byte : prefix.view()) { code.append(byte); }
  tail(code, point);
  return code;
}

void indexed_cases() {
  std::array<Byte, address_bytes> ram{};
  Core core(Memory::unified(ram));
  for (const auto form : indexed::forms()) {
    if (form.indirect) { continue; }
    for (const auto operation : operations) {
      const auto extra = operation.branch ? 2U : 1U;
      auto addressing = seed;
      addressing.pc = static_cast<std::uint16_t>(addressing.pc + extra);
      const auto address = indexed::pointer(form, addressing);
      for (const auto mask : values) {
        for (const auto data : values) {
          const Point point{.operation = operation,
                            .before = seed,
                            .updated = seed,
                            .address = address,
                            .data = data,
                            .mask = mask};
          one(core, ram, indexed_code(form, point), point);
        }
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Indexed bit operations include the complete mask/relative tail in PC-based addressing");
}

struct Auto {
  unsigned base;
  int delta;
  bool pre;
};

void auto_case(Core &core, const std::span<Byte> ram, const Auto change) {
  constexpr std::array<indexed::Field, 3> fields{&Registers::x, &Registers::y, &Registers::sp};
  const auto raw = change.delta > 0 ? static_cast<unsigned>(change.delta - 1)
                                    : 16U - static_cast<unsigned>(-change.delta);
  constexpr unsigned auto_mode = 0x20;
  constexpr unsigned post = 0x10;
  const auto postbyte =
      static_cast<Byte>((change.base << 6U) | auto_mode | (change.pre ? 0U : post) | raw);
  auto before = seed;
  constexpr std::uint16_t last = 0xffff;
  before.*fields[change.base] = change.pre ? 0 : last;
  auto updated = before;
  updated.*fields[change.base] =
      static_cast<std::uint16_t>(static_cast<int>(before.*fields[change.base]) + change.delta);
  const auto address = change.pre ? updated.*fields[change.base] : before.*fields[change.base];
  for (const auto operation : operations) {
    for (const auto mask : values) {
      for (const auto data : values) {
        const Point point{.operation = operation,
                          .before = before,
                          .updated = updated,
                          .address = address,
                          .data = data,
                          .mask = mask};
        Code code;
        code.append(operation.indexed);
        code.append(postbyte);
        tail(code, point);
        one(core, ram, code, point);
      }
    }
  }
}

void auto_cases() {
  std::array<Byte, address_bytes> ram{};
  Core core(Memory::unified(ram));
  for (unsigned base = 0; base < 3; ++base) {
    for (int delta = -8; delta <= 8; ++delta) {
      if (delta == 0) { continue; }
      for (const bool pre : {false, true}) {
        auto_case(core, ram, {.base = base, .delta = delta, .pre = pre});
      }
    }
  }
  require(core.statistics().read_callbacks == 0 && core.statistics().write_callbacks == 0,
          "Auto-index changes publish on both bit-branch outcomes without corrupting the native "
          "decision");
}

void capture_tail_cache() {
  std::array<Byte, address_bytes> ram{};
  Core core(Memory::unified(ram));
  constexpr std::uint16_t address = 0x2000;
  constexpr Byte word_offset = 0xe2;
  constexpr int positive_limit = 127;
  for (const auto mask : values) {
    for (int displacement = -128; displacement <= positive_limit; ++displacement) {
      const Point point{.operation = operations[2],
                        .before = seed,
                        .updated = seed,
                        .address = address,
                        .data = 0x55,
                        .mask = mask,
                        .displacement = displacement};
      Code code{.bytes = {point.operation.indexed, word_offset, 0x20, 0}, .length = 4};
      tail(code, point);
      one(core, ram, code, point);
    }
  }
  require(
      core.statistics().compiled_entries == values.size() * (byte_mask + 1U),
      "Distinct masks in byte5 and relative offsets in byte6 have complete nonaliasing cache keys");
  const auto before = core.registers();
  constexpr std::array<Byte, 7> last{0x0e, word_offset, 0x20, 0, 0xaa, 0x7f, 0};
  for (const unsigned length : {5U, 7U}) {
    const auto result = core.run_instruction(std::span(last).first(length));
    require(
        result.fault == (length == 7 ? Fault::invalid_input : Fault::unsupported_instruction) &&
            result.retired == 0 && core.registers() == before &&
            core.statistics().compiled_entries == values.size() * (byte_mask + 1U),
        "Warm six-byte entries cannot accept truncated or padded captures with the same prefix");
  }
}

void refusals() {
  Core core;
  for (const auto form : indexed::forms()) {
    if (!form.indirect) { continue; }
    for (const auto operation : operations) {
      const Point point{.operation = operation,
                        .before = seed,
                        .updated = seed,
                        .address = 0,
                        .data = 0,
                        .mask = 0};
      core.set_registers(seed);
      const auto result = core.run_instruction(indexed_code(form, point).view());
      require(result.fault == Fault::unsupported_instruction && result.retired == 0 &&
                  core.registers() == seed,
              "Every excluded indirect bit encoding refuses before pointer or operand access");
    }
  }
  constexpr std::array<Byte, 7> code{0x0e, 0xe2, 0, 0, 0, 0, 0};
  for (const unsigned size : {1U, 2U, 3U, 4U, 5U, 7U}) {
    core.set_registers(seed);
    const auto result = core.run_instruction(std::span(code).first(size));
    require(result.fault == (size > maximum_capture_bytes ? Fault::invalid_input
                                                          : Fault::unsupported_instruction) &&
                result.retired == 0 && core.registers() == seed,
            "Truncated or oversized bit captures cannot retire or alias an existing native entry");
  }
  require(core.statistics().compiled_entries == 0 && core.statistics().read_callbacks == 0 &&
              core.statistics().write_callbacks == 0,
          "Malformed and excluded bit captures have no native or external effects");
}
}

int main() {
  try {
    constexpr std::uint16_t last_address = 0xffff;
    complete_domain({.address = last_address, .direct = false, .partial = false});
    complete_domain(
        {.address = static_cast<std::uint16_t>(byte_mask), .direct = true, .partial = true});
    flag_domain();
    indexed_cases();
    auto_cases();
    capture_tail_cache();
    external_cases();
    refusals();
    std::cout << "Verified " << checked
              << " native bit cases, complete masks/values, NZV/unchanged flags, indexed PC tails, "
                 "six-byte cache keys and real bus faults.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
