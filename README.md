# cpu12

Independent CPU12/HC12 native JIT. C++23, Linux x86-64,
macOS ARM64, GPL-3.0-only. See OBJECTIVE.md for complete-ISA acceptance.

## Build

LLVM 19.1 development libraries, CMake, Ninja, clang-format-19 and clang-tidy-19
are required. Linux packages include `llvm-19-dev`; Homebrew `llvm@19` contains
the compiler, headers and shared library.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target bench
```

`strict` enables ASan/UBSan; `thread` selects separate TSan. Ccache is used when
available. Use packaged dependencies; third-party source is not built or sanitized.
On macOS use `brew install llvm@19 llvm ninja ccache`. The compiler remains LLVM 19;
for current macOS versions select the packaged current Clang sanitizer runtime:

```sh
export LLVM19_RESOURCE_DIR="$("$(brew --prefix llvm)/bin/clang" -print-resource-dir)"
cmake --preset strict
```

## Preparation

The [target architecture](doc/architecture.md) requires complete Ahead-of-Time
Binary Translation of consumer-owned executable images and known variants.
Prefer startup loading and complete preparation before execution. Support
build-time export of linkable LLVM bitcode or relocatable native objects through
the same semantic translation and optimization pipeline.
Development always runs complete AOT/optimization. Preserve validated code
artifacts for operational startup reuse, with exact code/binding/host identity
and safe relocation. Rebuild rejected artifacts before execution.

`Core::prepare(entries)` analyzes an immutable code/mapping view without guest
execution, real I/O or architectural state/time changes. Lift the complete known
executable program into semantic LLVM 19 IR, optimize connected functions and
loops with target-aware O3, and install code against validated bindings.
Fuse compatible consumer-owned host IR before optimization. Keep LLVM handles
private and original memory/device effects ordered.

Require complete translation before execution; report a gap with code identity,
address and reason. Distinguish total executable coverage from optimization
coverage. Include interrupt/indirect entries and known loader/template variants;
preserve original writes while selecting prepared native code. Keep mutable data
in borrowed host RAM and retain observable program words. Compilation and
validated artifact loading run on a stopped owner; code/cache identity includes
input hashes, binding ABI, compiler and host target/features.

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
  const std::array entries{cpu12::Address{0}};
  const auto prepared = core.prepare(entries);
  if (prepared.failure.fault != cpu12::Fault::none || prepared.unresolved_entries != 0) { return 1; }
  const auto result = core.run();
  return result.fault == cpu12::Fault::none ? 0 : 1;
}
```

Memory belongs to the consumer and must outlive Core. One thread owns execution;
state inspection is optional. Use Io for device effects and run_until() for an
absolute local cycle deadline. Core does not schedule an instrument or its audio.
Unsupported operations fail explicitly.

Benchmarks check independent results and report cold/warm timing and matched
host ratios. Continuous checksum and FFT runs also verify portable C++ host
references compiled with Clang 19 `-O3`, reporting `host_cpp_o3_s` and
`jit_to_cpp_o3_percent` separately from the retained ASM comparison and gates.
Require explicit LLVM O3 preparation and report its complete executable coverage
separately from warm timing and host/API costs.

JIT/host ratios use `100 * host_time / jit_time`. All scenarios have x86-64 and
ARM64 host references. Hardware factors use stated models, not measured device
speed. Warm populations use a fixed 100 ms warm-up after cold measurement;
packet warm-up advances both engines continuously. Select cases with
CPU12_BENCH_FILTER; fixtures are in test/bench/.
See doc/architecture.md and test/ for details.

[License](LICENSE)

LLVM dependency notices are retained in [THIRD_PARTY.txt](THIRD_PARTY.txt).
