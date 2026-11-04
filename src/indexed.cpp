// SPDX-License-Identifier: GPL-3.0-only
#include "instruction.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace cpu12::detail {
bool decode_indexed(Instruction &instruction, const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 2) { return false; }
  constexpr unsigned small_mode = 0x20;
  constexpr unsigned extended_mask = 0xe0;
  constexpr unsigned base_shift = 3;
  constexpr unsigned byte_bits = 8;
  const auto postbyte = bytes[1];
  Indexed indexed{.postbyte = postbyte};
  unsigned length = 2;
  if ((postbyte & small_mode) == 0) {
    constexpr unsigned small_bits = 5;
    constexpr unsigned small_mask = 31;
    constexpr unsigned small_extension = 0xffe0;
    indexed.base = static_cast<IndexBase>(postbyte >> 6U);
    const auto raw = postbyte & small_mask;
    indexed.displacement = static_cast<std::uint16_t>(
        (raw & (1U << (small_bits - 1U))) != 0 ? raw | small_extension : raw);
  } else {
    // Auto pre/post-update has a separate architectural commit order. Until
    // implemented, its legal encodings refuse before touching CPU or bus state.
    if ((postbyte & extended_mask) != extended_mask) { return false; }
    indexed.base = static_cast<IndexBase>((postbyte >> base_shift) & 3U);
    const auto form = postbyte & 7U;
    if (form < 2) {
      length = 3;
      if (bytes.size() != length) { return false; }
      constexpr unsigned byte_extension = 0xff00;
      indexed.displacement =
          static_cast<std::uint16_t>(((form & 1U) != 0 ? byte_extension : 0U) | bytes[2]);
    } else if (form < 4) {
      length = 4;
      if (bytes.size() != length) { return false; }
      indexed.displacement =
          static_cast<std::uint16_t>((static_cast<unsigned>(bytes[2]) << byte_bits) | bytes[3]);
      indexed.indirect = form == 3;
    } else {
      constexpr std::array offsets{IndexOffset::a, IndexOffset::b, IndexOffset::d, IndexOffset::d};
      indexed.offset = offsets[form & 3U];
      indexed.indirect = form == 7;
    }
  }
  if (bytes.size() != length) { return false; }
  instruction.indexed = indexed;
  instruction.addressing = Addressing::indexed;
  instruction.length = static_cast<std::uint8_t>(length);
  return true;
}
}
