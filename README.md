# cpu12

Independent CPU12/HC12 emulator with an x86-64 JIT.

C++23 · Linux x86-64 · GPL-3.0-only

## Build

Requires LLVM 19, CMake, Ninja, clang-format-19 and clang-tidy-19.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The `strict` preset enables ASan/UBSan.

## Use

```cmake
add_subdirectory(path/to/cpu12)
target_link_libraries(your_app PRIVATE cpu12::core)
```

```cpp
#include <cpu12/core.hpp>
#include <cpu12/memory.hpp>

#include <algorithm>
#include <array>
#include <span>

int main() {
  std::array<cpu12::Byte, cpu12::address_bytes> ram{};
  constexpr std::array<cpu12::Byte, 3> program{0xa7, 0x18, 0x3e}; // NOP; STOP
  std::ranges::copy(program, ram.begin());
  const std::array regions{
      cpu12::Ram{.bytes = std::span(ram).first(program.size()), .writable = false},
      cpu12::Ram{.first = static_cast<cpu12::Address>(program.size()),
                 .bytes = std::span(ram).subspan(program.size())}};
  cpu12::Core core(cpu12::Memory::mapped(regions));
  const auto result = core.run();
  return result.fault == cpu12::Fault::none ? 0 : 1;
}
```

Memory is borrowed and must outlive the core. One execution thread owns each
core. The example executes NOP and STOP; a consumer supplies its own program and
optional I/O. `state()` provides an optional live, read-only view of Core-owned
state on that execution thread. Unsupported instructions or modes report explicit faults.

`Memory::mapped` accepts disjoint `Ram` grants with optional live byte knownness
and write permission. Core copies the descriptors; their backing storage must
remain alive for its lifetime. Initialized read-only program grants enable
retained native blocks and backedges without repeated instruction fetches.
Writable or dynamically known program bytes retain exact captured-instruction
execution. Read-only program contents stay immutable for their binding identity.
`set_memory` selects bindings between runs while
preserving CPU state and retaining previous native translations. Changed executable
contents require a new grant `identity` at that transition. Boundary words
may require an owner word callback. Cycle/event accounting remains incomplete.

## Benchmarks

```sh
cmake --build --preset release --target bench
```

The command runs checked ALU/RAM/control-flow, indexed checksum, fixed-point
Mandelbrot, FFT and continuous checksum/FFT packet scenarios sequentially. Select
scenarios with `CPU12_BENCH_FILTER` (a CMake cache test-name regex).

Each scenario reports `hardware_factor` (target >=1) and `jit_to_host_percent` (target
>=70). The hardware estimate conservatively assumes a 30 MHz CPU retiring one
instruction per cycle; it is not a calibrated Blofeld timing model. The host
comparison uses handwritten x86-64 ASM with identical inputs and independent
state/memory expectations. Equal throughput is 100%. Finite-kernel timings include
setup and result checks on both sides; the percentage is `100 * host_time / jit_time`.
Cold compilation time is separate; warm execution must reuse translations.
Packet reports include actual CPU affinity and starting/ending CPUs. Use
`taskset` for placement diagnostics; pinned results do not qualify unrestricted
scheduling.

The finite kernels diagnose computation and call overhead. Their cold factors
compare one kernel to its ideal device execution time, not to an audio/MIDI packet
deadline. Short reset/run/check cases alone neither qualify sustained instrument
performance nor establish freedom from live underruns. Sustained integration
requires packet deadline measurements, including first-use compilation.

`packet-word-checksum` runs a continuous guest loop with persistent registers and
RAM: 7,499 big-endian words and 29,999 retired instructions per 1 ms packet, at
1,000 packets per virtual second. It reports cold/warm deadlines and nine warm
trials against host ASM; full state and memory checks run outside timing. Release
enforces the 70% comparison and packet deadlines; sanitizer builds only report
performance. This covers checksum processing only; other workloads require
their own sustained evidence. Run its directed result cases
with `cpu12-continuous-checksum-bench --verify`.

`packet-fft-stream` runs six 16-point Q8.8 FFTs per 48 kHz/16 processing packet:
333.3 microseconds per packet and 27.702 million guest instructions per virtual
second. Guest code advances a RAM word-ramp seed, fills bit-reversed input lanes,
transforms them and jumps back without STOP or host restoration. Host ASM preserves
all input, scratch and output stores. Independent state/memory cases include
signed word wrap and every retained CCR bit. This synthetic six-stream load is
not a Blofeld cycle model; cold compilation and warm processing must meet the
same explicit deadline/throughput gates.

[License](LICENSE)
