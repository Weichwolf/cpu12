// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "code_image.hpp"
#include "cpu12/core.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <string_view>

namespace cpu12::test::image {
constexpr std::uint64_t first_id = 7, second_id = 9;
constexpr unsigned declared_sites = 20;
constexpr Address vector = 0xffe0;
constexpr std::array interrupts{InterruptRequest{.vector = vector},
                                InterruptRequest{.vector = vector, .kind = InterruptKind::xirq}};

inline NativeImage compile() {
  const Source first(0x80);
  const Source second(0);
  const std::array variants{first.variant(first_id), second.variant(second_id)};
  const std::array layouts{VariantLayout{.id = first_id, .memory = layout},
                           VariantLayout{.id = second_id, .memory = layout}};
  return Core::compile({.variants = variants},
                       {.memory = layouts, .interrupts = interrupts, .transitions = {}});
}

template <class Action> void rejects(Action action, const std::string_view reason) {
  bool failed = false;
  try {
    action();
  } catch (const std::exception &error) {
    failed = std::string_view(error.what()).contains(reason);
  }
  require(failed, "An invalid public image operation must fail with its actual contract reason");
}

void image_contracts();
void image_events();
void image_irq_bus();
void image_deadlines();
void image_io();
}
