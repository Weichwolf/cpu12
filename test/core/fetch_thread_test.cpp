// SPDX-License-Identifier: GPL-3.0-only
#include "cpu12/core.hpp"
#include "cpu12/memory.hpp"
#include "fetch.hpp"
#include "runtime.hpp"

#include <array>
#include <barrier>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace cpu12;
constexpr Address start = 0xfffd;

struct Shape {
  std::array<Byte, maximum_capture_bytes> bytes;
  std::size_t length;
};

// Primary EMUL, secondary EMULS, immediate LDD and extended indexed LDD.
constexpr std::array<Shape, 4> shapes{Shape{.bytes = {0x13}, .length = 1},
                                      Shape{.bytes = {0x18, 0x13}, .length = 2},
                                      Shape{.bytes = {0xcc, 0x7f, 0xff}, .length = 3},
                                      Shape{.bytes = {0xec, 0xe2, 0x80, 0}, .length = 4}};

struct Source {
  Shape shape;
  std::size_t calls = 0;

  static bool read(void *const owner, const Address address, Byte &value) noexcept {
    auto &source = *static_cast<Source *>(owner);
    const auto offset = static_cast<Address>(address - start);
    ++source.calls;
    if (offset >= source.shape.length) { return false; }
    value = source.shape.bytes[offset];
    return true;
  }
};

void worker(const std::size_t index) {
  constexpr unsigned repetitions = 16;
  for (unsigned repeat = 0; repeat < repetitions; ++repeat) {
    for (std::size_t offset = 0; offset < shapes.size(); ++offset) {
      Source source{.shape = shapes[(index + offset) % shapes.size()]};
      detail::NativeContext context{.io = {.owner = &source, .fetch = Source::read}};
      const auto capture = detail::fetch(Memory::external(), context, start);
      if (!capture.supported || context.fault != Fault::none ||
          capture.size != source.shape.length || source.calls != source.shape.length ||
          capture.bytes != source.shape.bytes) {
        throw std::runtime_error(
            "Concurrent first/hit fetch must retain opcode-page length and exact bytes");
      }
    }
  }
}
}

int main() {
  try {
    std::barrier ready(2);
    std::array<std::exception_ptr, 2> errors{};
    std::array<std::jthread, 2> threads;
    for (std::size_t index = 0; index < threads.size(); ++index) {
      threads[index] = std::jthread([&, index] {
        ready.arrive_and_wait();
        try {
          worker(index);
        } catch (...) { errors[index] = std::current_exception(); }
      });
    }
    for (auto &thread : threads) { thread.join(); }
    for (const auto &error : errors) {
      if (error) { std::rethrow_exception(error); }
    }
    std::cout
        << "Concurrent independent opcode-page caches and wrapped instruction fetches passed\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
