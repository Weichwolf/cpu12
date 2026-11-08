// SPDX-License-Identifier: GPL-3.0-only
#include "jit/a64/code.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace cpu12::detail::a64 {
namespace {
constexpr unsigned register_shift = 5, operand_shift = 16, immediate_shift = 10;
constexpr unsigned half_bits = 16, byte_bits = 8, byte_mask = 0xff;
constexpr unsigned instruction_bytes = 4, condition_shift = 12;
constexpr unsigned branch_bits = 26, conditional_bits = 19;
constexpr std::uint32_t wide_bit = 0x80000000;

constexpr auto reg(const Reg value) {
  return static_cast<std::uint32_t>(value);
}

constexpr auto wide(const Width width) {
  return width == Width::x64 ? wide_bit : 0U;
}

constexpr unsigned bit_count(const Width width) {
  return width == Width::x64 ? 64U : 32U;
}
}

void Code::read_register(const Reg value) {
  read_ |= std::uint32_t{1} << reg(value);
}

void Code::write_register(const Reg value) {
  written_ |= std::uint32_t{1} << reg(value);
}

void Code::word(const std::uint32_t value) {
  const auto offset = bytes_.size();
  bytes_.resize(offset + instruction_bytes);
  for (unsigned index = 0; index < instruction_bytes; ++index) {
    bytes_[offset + index] = static_cast<std::uint8_t>((value >> (index * byte_bits)) & byte_mask);
  }
}

void Code::splice(const std::size_t offset, const std::span<const std::uint8_t> bytes) {
  if (offset > bytes_.size() || offset % instruction_bytes != 0 ||
      bytes.size() % instruction_bytes != 0) {
    throw std::invalid_argument("Invalid A64 code insertion extent");
  }
  bytes_.insert(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin(), bytes.end());
}

void Code::append(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() % instruction_bytes != 0) {
    throw std::invalid_argument("A64 code must contain complete instructions");
  }
  bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
}

void Code::align(const unsigned alignment, const std::size_t offset) {
  if (alignment < instruction_bytes || !std::has_single_bit(alignment) ||
      offset % instruction_bytes != 0) {
    throw std::invalid_argument("Invalid A64 code alignment");
  }
  constexpr std::uint32_t nop = 0xd503201f;
  while ((size() + offset) % alignment != 0) { word(nop); }
}

void Code::constant(const Reg target, const std::uint64_t value, const Width width) {
  write_register(target);

  if (width == Width::w32 && value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("A64 constant exceeds its register width");
  }
  constexpr std::uint32_t movz = 0x52800000;
  constexpr std::uint32_t movk = 0x72800000;
  constexpr unsigned lane_shift = 21;
  constexpr unsigned half_mask = 0xffff;
  const auto first = value == 0 ? 0U : static_cast<unsigned>(std::countr_zero(value)) / half_bits;
  for (unsigned lane = first; lane < bit_count(width) / half_bits; ++lane) {
    const auto part = static_cast<std::uint32_t>((value >> (lane * half_bits)) & half_mask);
    if (lane != first && part == 0) { continue; }
    word(wide(width) | (lane == first ? movz : movk) | (lane << lane_shift) |
         (part << register_shift) | reg(target));
  }
}

void Code::move(const Reg target, const Reg source, const Width width) {
  alu(Alu::bit_or, target, Reg::zr, source, width);
}

void Code::alu(
    const Alu operation, const Reg target, const Reg left, const Reg right, const Width width) {
  read_register(left);
  read_register(right);
  write_register(target);

  word(wide(width) | static_cast<std::uint32_t>(operation) | (reg(right) << operand_shift) |
       (reg(left) << register_shift) | reg(target));
}

void Code::add(const Reg target, const Reg source, const std::uint16_t value, const Width width) {
  read_register(source);
  write_register(target);

  constexpr std::uint32_t opcode = 0x11000000;
  constexpr unsigned maximum = 0xfff;
  if (value > maximum) { throw std::invalid_argument("A64 add immediate exceeds 12 bits"); }
  word(wide(width) | opcode | (std::uint32_t{value} << immediate_shift) |
       (reg(source) << register_shift) | reg(target));
}

void Code::subtract(const Reg target,
                    const Reg source,
                    const std::uint16_t value,
                    const Width width) {
  read_register(source);
  write_register(target);

  constexpr unsigned maximum = 0xfff;
  if (value > maximum) { throw std::invalid_argument("A64 subtract immediate exceeds 12 bits"); }
  constexpr std::uint32_t opcode = 0x51000000;
  word(wide(width) | opcode | (std::uint32_t{value} << immediate_shift) |
       (reg(source) << register_shift) | reg(target));
}

void Code::adjust(const Reg target,
                  const Reg source,
                  const std::uint64_t value,
                  const Reg temporary,
                  const Width width,
                  const bool negative) {
  constexpr unsigned maximum_immediate = 4095;
  if (value <= maximum_immediate) {
    if (negative) {
      subtract(target, source, static_cast<std::uint16_t>(value), width);
    } else {
      add(target, source, static_cast<std::uint16_t>(value), width);
    }
  } else {
    if (source == temporary) {
      throw std::invalid_argument("A64 adjustment scratch aliases input");
    }
    constant(temporary, value, width);
    alu(negative ? Alu::subtract : Alu::add, target, source, temporary, width);
  }
}

void Code::add(const Reg target,
               const Reg source,
               const std::uint64_t value,
               const Reg temporary,
               const Width width) {
  adjust(target, source, value, temporary, width, false);
}

void Code::subtract(const Reg target,
                    const Reg source,
                    const std::uint64_t value,
                    const Reg temporary,
                    const Width width) {
  adjust(target, source, value, temporary, width, true);
}

void Code::insert(const Reg target,
                  const Reg source,
                  const unsigned first,
                  const unsigned count,
                  const Width width) {
  read_register(source);
  read_register(target);
  write_register(target);

  const auto bits = bit_count(width);
  if (count == 0 || first >= bits || count > bits - first) {
    throw std::invalid_argument("Invalid A64 insertion extent");
  }
  constexpr std::uint32_t narrow = 0x33000000;
  constexpr std::uint32_t full = 0xb3400000;
  const auto opcode = width == Width::x64 ? full : narrow;
  word(opcode | (((bits - first) % bits) << operand_shift) | ((count - 1) << immediate_shift) |
       (reg(source) << register_shift) | reg(target));
}

void Code::scalar_move(const Reg value, const unsigned vector, const bool to_vector) {
  if (to_vector) {
    read_register(value);
  } else {
    write_register(value);
  }

  constexpr unsigned vector_registers = 32;
  if (vector >= vector_registers) { throw std::invalid_argument("Invalid A64 vector register"); }
  constexpr std::uint32_t to_scalar = 0x1e270000;
  constexpr std::uint32_t from_scalar = 0x1e260000;
  word(to_vector ? to_scalar | (reg(value) << register_shift) | vector
                 : from_scalar | (vector << register_shift) | reg(value));
}

void Code::extract(const Reg target,
                   const Reg source,
                   const unsigned first,
                   const unsigned count,
                   const Width width,
                   const bool sign) {
  read_register(source);
  write_register(target);

  const auto bits = bit_count(width);
  if (count == 0 || first >= bits || count > bits - first) {
    throw std::invalid_argument("Invalid A64 bit-field extent");
  }
  constexpr std::uint32_t ubfm = 0x53000000;
  constexpr std::uint32_t sbfm = 0x13000000;
  constexpr std::uint32_t wide_n = 0x00400000;
  word(wide(width) | (sign ? sbfm : ubfm) | (width == Width::x64 ? wide_n : 0U) |
       (first << operand_shift) | ((first + count - 1) << immediate_shift) |
       (reg(source) << register_shift) | reg(target));
}

void Code::shift(const Shift operation,
                 const Reg target,
                 const Reg source,
                 const unsigned count,
                 const Width width) {
  read_register(source);
  write_register(target);

  const auto bits = bit_count(width);
  if (count >= bits) { throw std::invalid_argument("A64 shift exceeds register width"); }
  if (operation == Shift::right || operation == Shift::arithmetic) {
    extract(target, source, count, bits - count, width, operation == Shift::arithmetic);
    return;
  }
  constexpr std::uint32_t ubfm = 0x53000000;
  constexpr std::uint32_t extr = 0x13800000;
  constexpr std::uint32_t wide_n = 0x00400000;
  const auto encoding = wide(width) | (width == Width::x64 ? wide_n : 0U);
  if (operation == Shift::left) {
    word(encoding | ubfm | (((bits - count) % bits) << operand_shift) |
         ((bits - count - 1) << immediate_shift) | (reg(source) << register_shift) | reg(target));
  } else {
    word(encoding | extr | (reg(source) << operand_shift) | (count << immediate_shift) |
         (reg(source) << register_shift) | reg(target));
  }
}

void Code::reverse_half(const Reg target, const Reg source, const Width width) {
  read_register(source);
  write_register(target);

  constexpr std::uint32_t opcode = 0x5ac00400;
  word(wide(width) | opcode | (reg(source) << register_shift) | reg(target));
}

void Code::reverse_bytes(const Reg target, const Reg source, const Width width) {
  read_register(source);
  write_register(target);
  constexpr std::uint32_t narrow = 0x5ac00800;
  constexpr std::uint32_t full = 0xdac00c00;
  word((width == Width::x64 ? full : narrow) | (reg(source) << register_shift) | reg(target));
}

void Code::memory(const Reg value,
                  const Reg base,
                  const std::size_t offset,
                  const Access access,
                  const bool read) {
  read_register(base);
  if (read) {
    write_register(value);
  } else {
    read_register(value);
  }

  constexpr std::uint32_t store = 0x39000000;
  constexpr std::uint32_t load = 0x00400000;
  constexpr unsigned size_shift = 30;
  constexpr unsigned maximum_offset = 0xfff;
  const auto size = static_cast<unsigned>(access);
  const auto alignment = std::size_t{1} << size;
  if (offset % alignment != 0 || offset / alignment > maximum_offset) {
    throw std::invalid_argument("Unaligned or oversized A64 load/store offset");
  }
  word(store | (read ? load : 0U) | (size << size_shift) |
       (static_cast<std::uint32_t>(offset / alignment) << immediate_shift) |
       (reg(base) << register_shift) | reg(value));
}

void Code::load(const Reg target, const Reg base, const std::size_t offset, const Access access) {
  memory(target, base, offset, access, true);
}

void Code::store(const Reg source, const Reg base, const std::size_t offset, const Access access) {
  memory(source, base, offset, access, false);
}

void Code::multiply(const Reg target, const Reg left, const Reg right, const Width width) {
  read_register(left);
  read_register(right);
  write_register(target);

  constexpr std::uint32_t opcode = 0x1b007c00;
  word(wide(width) | opcode | (reg(right) << operand_shift) | (reg(left) << register_shift) |
       reg(target));
}

void Code::multiply(const Reg target,
                    const Reg source,
                    const std::uint64_t factor,
                    const Reg temporary,
                    const Width width) {
  if (factor == 0) {
    constant(target, 0, width);
  } else if (factor == 1) {
    move(target, source, width);
  } else if (std::has_single_bit(factor)) {
    shift(Shift::left, target, source, static_cast<unsigned>(std::countr_zero(factor)), width);
  } else {
    if (source == temporary) {
      throw std::invalid_argument("A64 multiplier scratch aliases input");
    }
    constant(temporary, factor, width);
    multiply(target, source, temporary, width);
  }
}

void Code::divide(
    const Reg target, const Reg left, const Reg right, const bool sign, const Width width) {
  read_register(left);
  read_register(right);
  write_register(target);

  constexpr std::uint32_t unsigned_divide = 0x1ac00800;
  constexpr std::uint32_t signed_divide = 0x1ac00c00;
  word(wide(width) | (sign ? signed_divide : unsigned_divide) | (reg(right) << operand_shift) |
       (reg(left) << register_shift) | reg(target));
}

void Code::multiply_subtract(
    const Reg target, const Reg left, const Reg right, const Reg minuend, const Width width) {
  read_register(left);
  read_register(right);
  read_register(minuend);
  write_register(target);

  constexpr std::uint32_t opcode = 0x1b008000;
  word(wide(width) | opcode | (reg(right) << operand_shift) | (reg(minuend) << immediate_shift) |
       (reg(left) << register_shift) | reg(target));
}

void Code::compare(const Reg left, const Reg right, const Width width) {
  alu(Alu::subtract_flags, Reg::zr, left, right, width);
}

void Code::compare(const Reg source, const std::uint16_t value, const Width width) {
  constexpr unsigned maximum = 0xfff;
  if (value > maximum) { throw std::invalid_argument("A64 compare immediate exceeds 12 bits"); }
  read_register(source);
  constexpr std::uint32_t opcode = 0x7100001f;
  word(wide(width) | opcode | (std::uint32_t{value} << immediate_shift) |
       (reg(source) << register_shift));
}

void Code::compare_if(const Reg left,
                      const Reg right,
                      const Condition condition,
                      const Width width) {
  read_register(left);
  read_register(right);
  constexpr std::uint32_t opcode = 0x7a400000;
  word(wide(width) | opcode | (reg(right) << operand_shift) |
       (static_cast<unsigned>(condition) << condition_shift) | (reg(left) << register_shift));
}

void Code::select(const Reg target,
                  const Reg left,
                  const Reg right,
                  const Condition condition,
                  const Width width) {
  read_register(left);
  read_register(right);
  write_register(target);

  constexpr std::uint32_t opcode = 0x1a800000;
  word(wide(width) | opcode | (reg(right) << operand_shift) |
       (static_cast<unsigned>(condition) << condition_shift) | (reg(left) << register_shift) |
       reg(target));
}

void Code::set(const Reg target, const Condition condition) {
  write_register(target);

  constexpr std::uint32_t opcode = 0x1a9f07e0;
  word(opcode | ((static_cast<unsigned>(condition) ^ 1U) << condition_shift) | reg(target));
}

void Code::patch(const Reference reference, const std::size_t target) {
  if (reference.conditional) {
    const auto first = reference.offset - instruction_bytes;
    const auto distance = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(first);
    constexpr std::int64_t reach = 1 << 20;
    if (distance >= -reach && distance < reach) {
      std::uint32_t branch = 0;
      for (unsigned index = 0; index < instruction_bytes; ++index) {
        branch |= std::uint32_t{bytes_[first + index]} << (index * byte_bits);
      }
      constexpr std::uint32_t immediate = 0x00ffffe0;
      constexpr std::uint32_t condition = 0x54000000;
      const auto inversion = (branch & 0x7e000000) == condition ? 1U : 0x01000000U;
      branch = (branch & ~immediate) ^ inversion;
      constexpr std::uint32_t nop = 0xd503201f;
      for (unsigned index = 0; index < instruction_bytes; ++index) {
        bytes_[first + index] =
            static_cast<std::uint8_t>((branch >> (index * byte_bits)) & byte_mask);
        bytes_[reference.offset + index] =
            static_cast<std::uint8_t>((nop >> (index * byte_bits)) & byte_mask);
      }
      patch({.offset = first, .bits = conditional_bits}, target);
      return;
    }
  }
  const auto delta =
      static_cast<std::int64_t>(target) - static_cast<std::int64_t>(reference.offset);
  const auto limit = std::int64_t{1} << (reference.bits - 1);
  if (delta % instruction_bytes != 0 || delta / instruction_bytes < -limit ||
      delta / instruction_bytes >= limit) {
    throw std::length_error("A64 branch target exceeds immediate range");
  }
  const auto mask = (std::uint32_t{1} << reference.bits) - 1;
  const auto field = (std::bit_cast<std::uint64_t>(delta / instruction_bytes) & mask)
                     << (reference.bits == 26 ? 0U : register_shift);
  for (unsigned index = 0; index < instruction_bytes; ++index) {
    bytes_[reference.offset + index] |=
        static_cast<std::uint8_t>((field >> (index * byte_bits)) & byte_mask);
  }
}

void Code::reference(Label &label, const unsigned bits, const bool conditional) {
  const Reference pending{
      .offset = size() - instruction_bytes, .bits = bits, .conditional = conditional};
  if (label.bound) {
    patch(pending, label.offset);
  } else {
    label.references.push_back(pending);
  }
}

void Code::branch(Label &label) {
  constexpr std::uint32_t opcode = 0x14000000;
  constexpr unsigned bits = 26;
  word(opcode);
  reference(label, bits);
}

void Code::branch(const Condition condition, Label &label) {
  constexpr std::uint32_t opcode = 0x54000000;
  constexpr unsigned bits = 19;
  word(opcode | static_cast<unsigned>(condition));
  reference(label, bits);
}

void Code::branch_zero(const Reg value, Label &label, const Width width, const bool nonzero) {
  read_register(value);

  constexpr std::uint32_t opcode = 0x34000000;
  constexpr std::uint32_t nonzero_bit = 0x01000000;
  constexpr unsigned bits = 19;
  word(wide(width) | opcode | (nonzero ? nonzero_bit : 0U) | reg(value));
  reference(label, bits);
}

void Code::bind(Label &label) {
  if (label.bound) { throw std::logic_error("A64 label already bound"); }
  label.offset = size();
  label.bound = true;
  for (const auto pending : label.references) { patch(pending, label.offset); }
  label.references.clear();
}

void Code::branch_far(const Condition condition, Label &label) {
  if (condition == Condition::always) {
    branch(label);
    return;
  }
  constexpr std::uint32_t inverse = 0x54000040;
  constexpr std::uint32_t jump = 0x14000000;
  word(inverse | (static_cast<unsigned>(condition) ^ 1U));
  word(jump);
  reference(label, branch_bits, true);
}

void Code::branch_zero_far(const Reg value, Label &label, const Width width, const bool nonzero) {
  read_register(value);
  constexpr std::uint32_t inverse = 0x34000040;
  constexpr std::uint32_t nonzero_bit = 0x01000000;
  constexpr std::uint32_t jump = 0x14000000;
  word(wide(width) | inverse | (nonzero ? 0U : nonzero_bit) | reg(value));
  word(jump);
  reference(label, branch_bits, true);
}

void Code::call(const Reg target) {
  read_register(target);

  constexpr std::uint32_t opcode = 0xd63f0000;
  word(opcode | (reg(target) << register_shift));
}

void Code::jump(const Reg target) {
  read_register(target);

  constexpr std::uint32_t opcode = 0xd61f0000;
  word(opcode | (reg(target) << register_shift));
}

void Code::return_() {
  constexpr std::uint32_t opcode = 0xd65f03c0;
  word(opcode);
}

void Code::push(const Reg left, const Reg right) {
  read_register(left);
  read_register(right);

  constexpr std::uint32_t opcode = 0xa9bf03e0;
  word(opcode | (reg(right) << immediate_shift) | reg(left));
}

void Code::pop(const Reg left, const Reg right) {
  write_register(left);
  write_register(right);

  constexpr std::uint32_t opcode = 0xa8c103e0;
  word(opcode | (reg(right) << immediate_shift) | reg(left));
}

void Code::read_flags(const Reg target) {
  write_register(target);

  constexpr std::uint32_t opcode = 0xd53b4200;
  word(opcode | reg(target));
}

void Code::write_flags(const Reg source) {
  read_register(source);

  constexpr std::uint32_t opcode = 0xd51b4200;
  word(opcode | reg(source));
}
}
