// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "fetch_model.hpp"
#include "runtime.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <span>

namespace {
using namespace cpu12;
using namespace cpu12::test::fetching;
std::array<bool, 512> forms{};
constexpr unsigned page_prefix = 0x18;
constexpr unsigned extended_delta = 0x30;

struct Shape {
  unsigned opcode, length;
  bool page = false, indirect = true;
};

struct Source {
  std::array<Byte, maximum_capture_bytes> bytes{};
  unsigned length = 0, calls = 0;
  static constexpr Address start = 0xfffd;

  static bool read(void *owner, const Address address, Byte &value) noexcept {
    auto &source = *static_cast<Source *>(owner);
    const auto offset = static_cast<Address>(address - start);
    ++source.calls;
    if (offset >= source.length) { return false; }
    value = source.bytes[offset];
    return true;
  }
};

void capture(const std::span<const Byte> code, const bool accepted = true) {
  Source source{.length = static_cast<unsigned>(code.size())};
  std::ranges::copy(code, source.bytes.begin());
  cpu12::detail::NativeContext context{.io = {.owner = &source, .fetch = Source::read}};
  const auto result = cpu12::detail::fetch({}, context, Source::start);
  require(context.fault == Fault::none && result.supported == accepted &&
              result.size == code.size() && source.calls == code.size() &&
              context.fetch_callbacks == code.size() &&
              std::ranges::equal(std::span(result.bytes).first(result.size), code),
          "Independent instruction shape fetches exactly its bytes, in order, across PC wrap");
  ++captures;
}

void fixed(const Shape shape) {
  std::array<Byte, maximum_capture_bytes> code{};
  code[0] = static_cast<Byte>(shape.page ? page_prefix : shape.opcode);
  if (shape.page) { code[1] = static_cast<Byte>(shape.opcode); }
  forms[shape.opcode + (shape.page ? 256U : 0U)] = true;
  capture(std::span(code).first(shape.length));
}

void indexed(const Shape shape) {
  forms[shape.opcode] = true;
  for (unsigned postbyte = 0; postbyte < 256; ++postbyte) {
    constexpr std::array<Byte, maximum_capture_bytes> payload{0, 0, 0xa5, 0x5a, 0x81, 0x7f};
    auto code = payload;
    code[0] = static_cast<Byte>(shape.opcode);
    code[1] = static_cast<Byte>(postbyte);
    // Literal ISA postbyte sets, independent of the production bit-mask reduction.
    constexpr std::array<unsigned, 8> one{0xe0, 0xe1, 0xe8, 0xe9, 0xf0, 0xf1, 0xf8, 0xf9};
    constexpr std::array<unsigned, 8> two{0xe2, 0xe3, 0xea, 0xeb, 0xf2, 0xf3, 0xfa, 0xfb};
    constexpr std::array<unsigned, 8> pointers{0xe3, 0xeb, 0xf3, 0xfb, 0xe7, 0xef, 0xf7, 0xff};
    const bool excluded = !shape.indirect && std::ranges::contains(pointers, postbyte);
    unsigned extra = 0;
    if (std::ranges::contains(one, postbyte)) { extra = 1; }
    if (std::ranges::contains(two, postbyte)) { extra = 2; }
    capture(std::span(code).first(excluded ? 2U : shape.length + extra), !excluded);
  }
}

void arithmetic_shapes() {
  constexpr std::array<unsigned, 20> bytes{0x80, 0x81, 0x82, 0x84, 0x85, 0x86, 0x88,
                                           0x89, 0x8a, 0x8b, 0xc0, 0xc1, 0xc2, 0xc4,
                                           0xc5, 0xc6, 0xc8, 0xc9, 0xca, 0xcb};
  constexpr std::array<unsigned, 10> words{
      0x83, 0x8c, 0x8d, 0x8e, 0x8f, 0xc3, 0xcc, 0xcd, 0xce, 0xcf};
  for (const auto opcode : bytes) {
    fixed({.opcode = opcode, .length = 2});
    fixed({.opcode = opcode + 0x10U, .length = 2});
    indexed({.opcode = opcode + 0x20U, .length = 2});
    fixed({.opcode = opcode + extended_delta, .length = 3});
  }
  for (const auto opcode : words) {
    fixed({.opcode = opcode, .length = 3});
    fixed({.opcode = opcode + 0x10U, .length = 2});
    indexed({.opcode = opcode + 0x20U, .length = 2});
    fixed({.opcode = opcode + extended_delta, .length = 3});
  }
}

void memory_shapes() {
  constexpr std::array<unsigned, 2> unary_range{0x60, 0x69};
  for (unsigned opcode = unary_range[0]; opcode <= unary_range[1]; ++opcode) {
    indexed({.opcode = opcode, .length = 2});
    fixed({.opcode = opcode + 0x10U, .length = 3});
  }
  constexpr unsigned indexed_test = 0xe7;
  constexpr unsigned extended_test = 0xf7;
  indexed({.opcode = indexed_test, .length = 2});
  fixed({.opcode = extended_test, .length = 3});
  constexpr std::array<unsigned, 2> store_range{0x5a, 0x5f};
  for (unsigned opcode = store_range[0]; opcode <= store_range[1]; ++opcode) {
    fixed({.opcode = opcode, .length = 2});
    indexed({.opcode = opcode + 0x10U, .length = 2});
    fixed({.opcode = opcode + 0x20U, .length = 3});
  }
  constexpr unsigned extended_bits = 0x1c;
  constexpr unsigned direct_bits = 0x4c;
  for (unsigned index = 0; index < 4; ++index) {
    const unsigned length = index < 2 ? 3U : 4U;
    indexed({.opcode = 0x0cU + index, .length = length, .indirect = false});
    fixed({.opcode = extended_bits + index, .length = length + 1});
    fixed({.opcode = direct_bits + index, .length = length});
  }
}

void simple_shapes() {
  constexpr std::array<unsigned, 14> inherent{
      2, 3, 8, 9, 0x11, 0x12, 0x13, 0x3d, 0x87, 0x97, 0xc7, 0xd7, 0xa7, 0x49};
  for (const auto opcode : inherent) { fixed({.opcode = opcode, .length = 1}); }
  constexpr std::array<std::array<unsigned, 2>, 3> ranges{
      {{0x30, 0x3b}, {0x40, 0x48}, {0x50, 0x59}}};
  for (const auto range : ranges) {
    for (unsigned opcode = range[0]; opcode <= range[1]; ++opcode) {
      fixed({.opcode = opcode, .length = 1});
    }
  }
  for (const unsigned opcode : {0x10U, 0x14U, 0xb7U, 7U, 0x17U}) {
    fixed({.opcode = opcode, .length = 2});
  }
  for (const unsigned opcode : {4U, 6U, 0x16U}) { fixed({.opcode = opcode, .length = 3}); }
  for (const unsigned opcode : {5U, 0x15U}) { indexed({.opcode = opcode, .length = 2}); }
  for (const unsigned opcode : {0x19U, 0x1aU, 0x1bU}) {
    indexed({.opcode = opcode, .length = 2, .indirect = false});
  }
  constexpr std::array<unsigned, 2> branches{0x20, 0x2f};
  for (unsigned opcode = branches[0]; opcode <= branches[1]; ++opcode) {
    fixed({.opcode = opcode, .length = 2});
    fixed({.opcode = opcode, .length = 4, .page = true});
  }
  constexpr std::array<unsigned, 11> page{6, 7, 14, 15, 16, 17, 19, 20, 21, 22, 23};
  for (const auto opcode : page) { fixed({.opcode = opcode, .length = 2, .page = true}); }
  constexpr std::array<unsigned, 6> word_lengths{5, 5, 4, 6, 6, 5};
  constexpr std::array<unsigned, 6> byte_lengths{4, 5, 4, 5, 6, 5};
  for (unsigned form = 0; form < word_lengths.size(); ++form) {
    fixed({.opcode = form, .length = word_lengths[form], .page = true});
    fixed({.opcode = form + 8, .length = byte_lengths[form], .page = true});
  }
}
}

int main() {
  try {
    arithmetic_shapes();
    memory_shapes();
    simple_shapes();
    constexpr unsigned supported_forms = 285;
    require(std::ranges::count(forms, true) == supported_forms,
            "Independent fetch catalogue covers all285 currently supported opcode forms");
    bus_cases();
    program_cases();
    std::cout << "Verified " << captures << " independent fetch shapes/boundaries and " << native
              << " native program retirements with bounded execution and explicit faults.\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
