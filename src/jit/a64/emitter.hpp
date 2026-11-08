// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/block.hpp"
#include "jit/interrupt.hpp"
#include "jit/resident.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cpu12::detail::a64 {
inline constexpr Reg registers = Reg::x19, context = Reg::x20, d = Reg::x21, x = Reg::x22,
                     y = Reg::x23, stack = Reg::x24, ccr = Reg::x25, pc = Reg::x26,
                     retired = Reg::x27, remaining = Reg::x28;
inline constexpr unsigned byte_bits = 8, word_bits = 16, byte_mask = 0xff, word_mask = 0xffff;
inline constexpr unsigned flag_c = 1, flag_v = 2, flag_z = 4, flag_n = 8, flag_i = 0x10,
                          flag_h = 0x20, flag_x = 0x40, flag_s = 0x80;

struct DataAccess {
  bool word = false, write = false, selected = false;
  Byte mask = 0;
};

enum class StateField : std::uint8_t {
  execution = offsetof(State, execution),
  irq_delay = offsetof(State, irq_delay),
  returned_frame = offsetof(State, returned_frame)
};

struct MemoryExit {
  Label &next, &done;
};

struct Flags {
  unsigned changed = flag_n | flag_z | flag_v;
  bool subtract = false, arithmetic = false;
};

class Emitter {
public:
  explicit Emitter(const Memory &memory) : memory_(memory) {}

  [[nodiscard]] std::vector<Byte> instruction(const Instruction &operation);
  [[nodiscard]] std::vector<Byte> block(std::span<const BlockInstruction> operations);
  [[nodiscard]] std::vector<Byte> resident_region(const ResidentRegion &region,
                                                  std::span<const Byte> general);
  [[nodiscard]] std::vector<Byte> resident(const ResidentPlan &plan,
                                           std::span<const BlockInstruction> suffix,
                                           std::span<const Byte> general);
  [[nodiscard]] std::vector<Byte>
  interrupt(InterruptRequest request, InterruptFrame frame, Byte next_pc);
  [[nodiscard]] std::vector<Byte> wake();

private:
  void begin(bool bounded);
  void publish();
  [[nodiscard]] std::vector<Byte> finish();
  void location(const Instruction &operation);
  void operation(const Instruction &instruction);
  void sequence(std::span<const BlockInstruction> operations);
  void commit(unsigned instructions, Progress elapsed);
  void clock(unsigned cycles);
  void control_clock(const Instruction &instruction);
  void yield_boundary();
  void resident_region_exit(const ResidentRegion &region, bool head);
  void resident_grant(const ResidentPlan &plan,
                      std::span<const BlockInstruction> suffix,
                      Label &general);
  void resident_indexed_grant(const ResidentPlan &plan, Label &general);
  void resident_prepare(const ResidentPlan &plan);
  void resident_accumulate(const ResidentOperation &operation, bool live);
  void resident_update(const ResidentOperation &operation, bool live);
  void resident_store(const ResidentOperation &operation);
  void resident_body(const ResidentPlan &plan, bool live);
  void resident_operation(const ResidentOperation &operation, bool live);
  [[nodiscard]] bool resident_fast(const ResidentOperation &operation);
  void resident_calculate(const ResidentOperation &operation);
  void resident_flags(const ResidentOperation &operation);
  void resident_value(Reg value, unsigned slot, bool read);
  void resident_memory(Reg value, const ResidentOperation &operation, bool write);
  void resident_multiply(const ResidentOperation &operation, bool live);
  void resident_operand(const ResidentOperation &operation, Reg value = Reg::x1);
  void advance(unsigned amount);
  void add_word(Reg target, Reg source, int amount);
  void mask(Reg target, Reg source, std::uint32_t bits);
  void load_target(Reg target, const Instruction &operation);
  void store_target(Reg value, const Instruction &operation);
  [[nodiscard]] static Reg word_target(WordTarget target);
  [[nodiscard]] static Reg index_base(IndexBase base);
  void address(const Instruction &operation);
  void commit_index(const Instruction &operation);
  void data_access(DataAccess access);
  void ram_access(Ram ram, DataAccess access, MemoryExit exits);
  void bus_access(DataAccess access);
  void operand(const Instruction &operation);
  void flags(unsigned bits, Flags policy = {});
  void arithmetic(const Instruction &operation);
  void unary(const Instruction &operation);
  void unary_value(const Instruction &operation);
  void special_register(const Instruction &operation);
  void decimal_adjust();
  void defer_interrupt(Reg candidate);
  void branch(const Instruction &operation);
  void branch_guard(const Instruction &operation, Label &not_taken);
  void loop(const Instruction &operation);
  void transfer(const Instruction &operation);
  void transfer_read(Reg value, unsigned selector, bool sign = false);
  void transfer_write(Reg value, unsigned selector);
  void memory_modify(const Instruction &operation);
  void move(const Instruction &operation);
  void stack_operation(const Instruction &operation);
  void subroutine(const Instruction &operation);
  void stack_context(Byte next_pc);
  void return_interrupt();
  void wait(const Instruction &operation);
  void math(const Instruction &operation);
  void divide(MathOp operation);
  void state_byte(StateField field, Byte value);
  unsigned resident_gpr_slots_ = 0;
  Reg resident_counter_ = Reg::zr;
  const ResidentOperation *resident_sum_ = nullptr;
  bool resident_sum_live_ = false;
  bool resident_index_elided_ = false;
  bool swapped_d_ = false;
  const Byte *resident_base_ = nullptr;
  Code code_;
  const Memory &memory_;
  Label failure_, done_;
};
}
