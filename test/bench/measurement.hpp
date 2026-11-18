// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <chrono>
#include <cstdint>
#include <stdexcept>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace cpu12::bench {
using Clock = std::chrono::steady_clock;

struct Measurement {
  double seconds = 0, instructions = 0, cycles = 0;
  std::uint64_t retired = 0;

  Measurement &operator+=(const Measurement &other) {
    seconds += other.seconds;
    instructions += other.instructions;
    cycles += other.cycles;
    retired += other.retired;
    return *this;
  }
};

class Counters {
public:
  explicit Counters(const bool enabled) {
#if defined(__linux__)
    if (!enabled) { return; }
    instructions_ = open(Event::instructions, -1);
    try {
      cycles_ = open(Event::cycles, instructions_);
    } catch (...) {
      close(instructions_);
      throw;
    }
#else
    static_cast<void>(enabled);
#endif
  }

  ~Counters() {
#if defined(__linux__)
    if (cycles_ >= 0) { close(cycles_); }
    if (instructions_ >= 0) { close(instructions_); }
#endif
  }

  Counters(const Counters &) = delete;
  Counters &operator=(const Counters &) = delete;

  [[nodiscard]] bool has_hardware_counters() const noexcept {
#if defined(__linux__)
    return instructions_ >= 0;
#else
    return false;
#endif
  }

  void start() {
#if defined(__linux__)
    if (instructions_ >= 0) {
      control(PERF_EVENT_IOC_RESET);
      control(PERF_EVENT_IOC_ENABLE);
    }
#endif
    start_ = Clock::now();
  }

  [[nodiscard]] Measurement stop() {
    const auto seconds = std::chrono::duration<double>(Clock::now() - start_).count();
    Measurement result{.seconds = seconds};
#if defined(__linux__)
    if (instructions_ >= 0) {
      control(PERF_EVENT_IOC_DISABLE);

      struct Record {
        std::uint64_t count, enabled, running, instructions, cycles;
      } record{};

      if (read(instructions_, &record, sizeof(record)) != sizeof(record) || record.count != 2 ||
          record.running == 0) {
        throw std::runtime_error("Cannot read owner hardware counter group");
      }
      const auto scale = static_cast<double>(record.enabled) / static_cast<double>(record.running);
      result.instructions = static_cast<double>(record.instructions) * scale;
      result.cycles = static_cast<double>(record.cycles) * scale;
    }
#endif
    return result;
  }

private:
#if defined(__linux__)
  enum class Event : std::uint8_t {
    instructions = PERF_COUNT_HW_INSTRUCTIONS,
    cycles = PERF_COUNT_HW_CPU_CYCLES
  };

  static int open(const Event event, const int group) {
    perf_event_attr attr{};
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = static_cast<std::uint64_t>(event);
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format =
        PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    const auto descriptor = syscall(SYS_perf_event_open, &attr, 0, -1, group, 0);
    if (descriptor < 0) { throw std::runtime_error("Owner hardware counters refused"); }
    return static_cast<int>(descriptor);
  }

  void control(const unsigned long request) const {
    if (ioctl(instructions_, request, PERF_IOC_FLAG_GROUP) != 0) {
      throw std::runtime_error("Cannot control owner hardware counters");
    }
  }

  int instructions_ = -1, cycles_ = -1;
#endif
  Clock::time_point start_;
};
}
