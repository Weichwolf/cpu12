// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace cpu12::detail {
namespace {
std::optional<unsigned> extended(Indexed &indexed, const std::span<const std::uint8_t> bytes) {
  constexpr unsigned base_shift = 3;
  constexpr unsigned byte_bits = 8;
  indexed.base = static_cast<IndexBase>((bytes[1] >> base_shift) & 3U);
  const auto form = bytes[1] & 7U;
  if (form < 2) {
    if (bytes.size() != 3) { return {}; }
    constexpr unsigned byte_extension = 0xff00;
    indexed.displacement =
        static_cast<std::uint16_t>(((form & 1U) != 0 ? byte_extension : 0U) | bytes[2]);
    return 3;
  }
  if (form < 4) {
    if (bytes.size() != 4) { return {}; }
    indexed.displacement =
        static_cast<std::uint16_t>((static_cast<unsigned>(bytes[2]) << byte_bits) | bytes[3]);
    indexed.indirect = form == 3;
    return 4;
  }
  constexpr std::array offsets{IndexOffset::a, IndexOffset::b, IndexOffset::d, IndexOffset::d};
  indexed.offset = offsets[form & 3U];
  indexed.indirect = form == 7;
  return 2;
}
}

bool decode_indexed(Instruction &instruction, const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 2) { return false; }
  constexpr unsigned small_mode = 0x20;
  constexpr unsigned extended_mask = 0xe0;
  const auto postbyte = bytes[1];
  Indexed indexed{.postbyte = postbyte};
  unsigned length = 2;
  if ((postbyte & small_mode) == 0) {
    constexpr unsigned sign = 16;
    constexpr unsigned small_mask = 31;
    constexpr unsigned small_extension = 0xffe0;
    indexed.base = static_cast<IndexBase>(postbyte >> 6U);
    const auto raw = postbyte & small_mask;
    indexed.displacement =
        static_cast<std::uint16_t>((raw & sign) != 0 ? raw | small_extension : raw);
  } else if ((postbyte & extended_mask) != extended_mask) {
    constexpr unsigned post = 0x10;
    constexpr unsigned update_mask = 15;
    indexed.base = static_cast<IndexBase>(postbyte >> 6U);
    const auto raw = static_cast<int>(postbyte & update_mask);
    indexed.update = static_cast<std::int8_t>(raw < 8 ? raw + 1 : raw - 16);
    indexed.displacement = (postbyte & post) == 0 ? static_cast<std::uint16_t>(indexed.update) : 0;
  } else {
    const auto count = extended(indexed, bytes);
    if (!count) { return false; }
    length = *count;
  }
  if (bytes.size() != length) { return false; }
  instruction.indexed = indexed;
  instruction.addressing = Addressing::indexed;
  instruction.length = static_cast<std::uint8_t>(length);
  return true;
}
}
