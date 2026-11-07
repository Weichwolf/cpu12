# cpu12

Independent CPU12/HC12 emulator with native x86-64 and ARM64 JITs.

C++23 · Linux x86-64 · macOS ARM64 · GPL-3.0-only

## Build

Requires LLVM 19, CMake, Ninja, clang-format-19 and clang-tidy-19.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

On macOS, install the matching tools with `brew install llvm@19 ninja ccache`.
Presets discover the Homebrew LLVM 19 installation without replacing system tools.
Both hosts use native code, direct mapped memory and retained immutable blocks.
I/O uses owner callbacks. Unsupported ISA forms report explicit faults.

The `strict` preset enables ASan/UBSan. On macOS 26.4 and later, LLVM 19's
original sanitizer runtime deadlocks during dyld initialization. Build the same
19.1.7 runtime with upstream fixes for [allocation-free shadow checks](https://github.com/llvm/llvm-project/commit/6a89439423351be2d63e13e191acd4cb33e0aaff)
and [dyld lookup](https://github.com/llvm/llvm-project/commit/2e7d07a33725a82ecfc514e27f047ece3ff13d4c):

```sh
export LLVM19_RESOURCE_DIR="$(python3 tools/darwin_runtime.py)"
cmake --preset strict
cmake --build --preset strict
```

This keeps the LLVM 19 compiler and headers, verifies source identities and retains
all sanitizers. Sources and runtime artifacts remain under ignored `build/local/`.
A second core checkout can use the same `LLVM19_RESOURCE_DIR`.

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
may require an owner word callback.

`progress()` reports completed nominal internal-bus cycles, including reset and
interrupt entries. `run_until()` uses an absolute nominal-cycle horizon and may
finish the final atomic instruction beyond it. It returns at interrupt-admission
changes; I/O can request return after its instruction with `request_yield()`.
External waits, clock routing and idle time belong to the consumer.

## Benchmarks

```sh
cmake --build --preset release --target bench
```

The benchmarks compare native JIT execution with handwritten host ASM and check
registers and memory against independent expectations. They report cold translation,
warm throughput, packet deadline misses and `jit_to_host_percent`
(`100 * host_time / jit_time`). The conservative `hardware_factor` assumes a
30 MHz CPU retiring one instruction per cycle; it is not a Blofeld timing model.

ALU, RAM, control flow, checksum, Mandelbrot and FFT comparisons support both
hosts. Continuous checksum uses `run_until`; continuous FFT uses
instruction-counted packets. Select scenarios with the CMake
cache variable `CPU12_BENCH_FILTER`. Fixture definitions are in [test/bench](test/bench).

[License](LICENSE)
