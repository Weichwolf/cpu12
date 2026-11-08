// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "instruction.hpp"
#include "jit/a64/code.hpp"
#include "jit/block.hpp"
#include "jit/emission.hpp"
#include "jit/interrupt.hpp"
#include "jit/resident.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace cpu12::detail::a64 {
inline constexpr Reg registers = Reg::x19, context = Reg::x20, d = Reg::x21, x = Reg::x22,
                     y = Reg::x23, stack = Reg::x24, ccr = Reg::x25, pc = Reg::x26,
                     retired = Reg::x27, remaining = Reg::x28;
inline constexpr unsigned byte_bits = 8, word_bits = 16, byte_mask = 0xff, word_mask = 0xffff;
inline constexpr unsigned flag_c = 1, flag_v = 2, flag_z = 4, flag_n = 8, flag_i = 0x10,
                          flag_h = 0x20, flag_x = 0x40, flag_s = 0x80;

inline constexpr std::array region_values{Reg::x7, Reg::x8, Reg::x9, Reg::x10, Reg::x12, Reg::x13};

enum class EntryMode : std::uint8_t { single, block, resident };
enum class Admission : std::uint8_t { checked, reserved };
enum class Publication : std::uint8_t { immediate, deferred };
enum class StoreMode : std::uint8_t { write, elided };
enum class ResidentMode : std::uint8_t { complete, bounded };
enum class RegionExit : std::uint8_t { head, tail, budget };

struct RegionLabels {
  Label repeat, head, tail, budget, final_tail, terminal, complete;
};

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
                                                  std::span<const Byte> general,
                                                  std::span<const BlockInstruction> terminal = {});
  [[nodiscard]] std::vector<Byte> resident(const ResidentPlan &plan,
                                           std::span<const BlockInstruction> suffix,
                                           std::span<const Byte> general,
                                           ResidentMode mode = ResidentMode::complete,
                                           std::span<const BlockInstruction> terminal = {});
  [[nodiscard]] std::vector<Byte>
  interrupt(InterruptRequest request, InterruptFrame frame, Byte next_pc);
  [[nodiscard]] std::vector<Byte> wake();

private:
  void begin(EntryMode mode);
  [[nodiscard]] Code prologue(std::uint32_t used) const;
  void publish();
  void publish_flags();
  [[nodiscard]] std::vector<Byte> finish();
  void location(const Instruction &operation);
  void record_exit(ExitLocation location);
  void operation(const Instruction &instruction);
  void sequence(std::span<const BlockInstruction> operations,
                Admission admission = Admission::checked);
  void commit(unsigned instructions, Progress elapsed, Admission admission = Admission::checked);
  void clock(unsigned cycles);
  void clock(unsigned cycles, Reg source);
  void commit_owner(const Instruction &instruction, Progress elapsed, Admission admission);
  void control_clock(const Instruction &instruction);
  void yield_boundary();
  void resident_region_cache(const ResidentPlan &plan);
  void resident_region_stores(const ResidentPlan &plan, std::size_t head);
  [[nodiscard]] bool resident_deferred(const ResidentOperation &operation) const;
  void resident_writeback(RegionExit exit);
  [[nodiscard]] Reg resident_word(const ResidentOperation &operation) const;
  void resident_region_exit(const ResidentRegion &region,
                            RegionExit exit,
                            Publication publication = Publication::immediate);
  [[nodiscard]] Reg
  resident_region_tail(const ResidentRegion &region, RegionLabels &labels, bool counted);
  [[nodiscard]] Reg resident_compare(const ResidentOperation &operation);
  void resident_compare_flags(const ResidentOperation &operation, Reg source);
  void resident_region_grant(const ResidentRegion &region,
                             std::span<const BlockInstruction> terminal,
                             Label &general);
  void resident_bounded_grant(const ResidentPlan &plan, Label &general);
  void resident_trips(const ResidentPlan &plan);
  void resident_grant(const ResidentPlan &plan,
                      std::span<const BlockInstruction> suffix,
                      Label &general);
  void resident_indexed_grant(const ResidentPlan &plan, Label &general);
  void resident_prepare(const ResidentPlan &plan);
  void resident_inputs(const ResidentPlan &plan);
  void resident_accumulate(const ResidentOperation &operation, bool live);
  void resident_update(const ResidentOperation &operation, bool live);
  [[nodiscard]] std::vector<StoreMode> resident_stores(const ResidentPlan &plan) const;
  void resident_store(const ResidentOperation &operation, StoreMode storage);
  void resident_cache_store(const ResidentOperation &operation, Reg value);
  void resident_body(const ResidentPlan &plan, bool live);
  void resident_suffix(std::span<const BlockInstruction> operations);
  void resident_operation(const ResidentOperation &operation,
                          bool live,
                          StoreMode storage = StoreMode::write);
  [[nodiscard]] bool resident_fast(const ResidentOperation &operation);
  void resident_calculate(const ResidentOperation &operation);
  void resident_flags(const ResidentOperation &operation);
  void resident_value(Reg value, unsigned slot, bool read);
  void resident_memory(Reg value, const ResidentOperation &operation, bool write);
  void resident_memory(Reg value, const ResidentOperation &operation, bool write, Access access);
  [[nodiscard]] bool resident_pair(const Byte *first, const Byte *second) const;
  [[nodiscard]] bool resident_product_store(const ResidentPlan &plan,
                                            std::size_t index,
                                            std::span<const StoreMode> stores,
                                            bool live);
  [[nodiscard]] bool resident_product(const ResidentPlan &plan,
                                      std::size_t index,
                                      bool live,
                                      std::span<const StoreMode> stores);
  [[nodiscard]] bool resident_y_live(const ResidentPlan &plan,
                                     std::size_t index,
                                     bool live,
                                     std::span<const StoreMode> stores) const;
  void resident_multiply(const ResidentOperation &operation,
                         bool live,
                         bool square = false,
                         std::array<Reg, 2> inputs = {d, y},
                         bool publish_y = true);
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
  [[nodiscard]] bool direct_ram(const Instruction &operation, bool write);
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
  [[nodiscard]] Reg frame_value(Reg source, bool word, Byte next_pc);
  void frame_ram(Byte next_pc);
  void frame_admission(Byte next_pc, Label &complete);
  void stack_context(Byte next_pc);
  void return_interrupt();
  void wait(const Instruction &operation);
  void math(const Instruction &operation);
  void divide(MathOp operation);
  void state_byte(StateField field, Byte value);
  EmissionOptions policy_;
  std::optional<ExitLocation> terminal_exit_;
  std::optional<std::size_t> loop_offset_;
  std::uint32_t input_registers_ = ~std::uint32_t{0};
  std::size_t begin_offset_ = 0;
  std::vector<const Byte *> resident_words_, head_stores_, tail_stores_;
  bool d_word_ = false;
  unsigned resident_gpr_slots_ = 0;
  Reg pending_progress_ = Reg::zr;
  Reg resident_counter_ = Reg::zr;
  Reg region_counter_ = Reg::zr;
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
