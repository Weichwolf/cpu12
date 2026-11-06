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
Mandelbrot and FFT scenarios sequentially. Select scenarios with the CMake cache
variable `CPU12_BENCH_FILTER` (a test-name regex).

Each row reports `hardware_factor` (target >=1) and `jit_to_host_percent` (target
>=70). The hardware estimate conservatively assumes a 30 MHz CPU retiring one
instruction per cycle; it is not a calibrated Blofeld timing model. The host
comparison uses handwritten x86-64 ASM with identical inputs and independent
state/memory expectations. Equal throughput is 100%; setup and result checks are
included on both sides. The percentage is `100 * host_time / jit_time`.
Cold compilation time is separate; warm execution must
reuse translations.

These finite kernels diagnose computation and call overhead. Their cold factors
compare one kernel to its ideal device execution time, not to an audio/MIDI packet
deadline. Short reset/run/check cases alone neither qualify sustained instrument
performance nor establish freedom from live underruns. Sustained integration
requires packet deadline measurements, including first-use compilation.

[License](LICENSE)
