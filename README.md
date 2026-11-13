# cpu12

Independent CPU12/HC12 native translator. C++23, Linux x86-64 and macOS ARM64,
GPL-3.0-only. [OBJECTIVE.md](OBJECTIVE.md) defines acceptance;
[doc/architecture.md](doc/architecture.md) defines the current target.

## Build

Use packaged LLVM19/Clang with development headers/shared library, llvm-extract,
llvm-link, CMake, Ninja, clang-format and clang-tidy. Linux uses llvm-19-dev;
macOS uses brew install llvm@19 llvm ninja ccache. No foreign source builds.

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target bench
```

Release selects optimized code without sanitizers. strict selects Debug with
ASan/UBSan; thread selects separate TSan. Preserve strict warnings/linking,
clang-tidy and formatting. Instrument our code/generated code, never packaged
foreign libraries. On current macOS, keep the LLVM19 compiler and select the
packaged current Clang sanitizer runtime:

```sh
export LLVM19_RESOURCE_DIR="$("$(brew --prefix llvm)/bin/clang" -print-resource-dir)"
cmake --preset strict
```

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
  constexpr std::array<cpu12::Byte, 3> program{0xa7, 0x20, 0xfe};
  constexpr std::array<cpu12::Address, 1> entries{0};
  const std::array code{cpu12::CodeSpan{.address = 0, .words = program}};
  const std::array variants{cpu12::CodeVariant{.id = 0, .code = code, .entries = entries}};
  const std::array layout{
      cpu12::RegionLayout{.first = 0, .size = program.size(), .writable = false},
      cpu12::RegionLayout{.first = program.size(),
                           .size = cpu12::address_bytes - program.size()}};
  const std::array layouts{cpu12::VariantLayout{.id = 0, .memory = layout}};
  const auto image = cpu12::Core::compile({.variants = variants},
                                         {.memory = layouts, .interrupts = {}});

  std::array<cpu12::Byte, cpu12::address_bytes> ram{};
  std::ranges::copy(program, ram.begin());
  const std::array regions{
      cpu12::Ram{.bytes = std::span(ram).first(program.size()), .writable = false},
      cpu12::Ram{.first = program.size(), .bytes = std::span(ram).subspan(program.size())}};
  cpu12::Core core(cpu12::Memory::mapped(regions));
  core.install(image, 0);
  const auto result = core.run(10);
  return result.fault == cpu12::Fault::none && result.retired == 10 ? 0 : 1;
}
```

Memory belongs to the consumer and must outlive Core. One owner executes a core.
Use public Io for devices and `run_until()` for an absolute local cycle deadline;
the library does not schedule an instrument or its audio.

Declare complete code streams and valid entries per variant, pointer-free memory
layouts and required IRQ kind/vector contracts. Keep inline readable data outside
instruction streams; a saved return address alone is not an executable entry.

Fixed topology is the default and requires complete per-variant layouts. Select
`CompileOptions::topology = MemoryTopology::dynamic` with no layouts when memory
will be remapped independently of the prepared code domains. Native accesses use
current grants with the same knownness, rights, byte/word/masked I/O and fault
semantics. Both modes require actual initialized immutable executable bytes at
installation; neither permits runtime translation.

`Core::compile` lowers, materializes and binds a shareable `NativeImage`.
`Core::install` selects a variant on the stopped owner, validates source/layout
and retains shared executable ownership without changing architecture or time.
`Core::install(image, domain_ids)` admits compatible dynamic domains together.
Their executable bytes must be disjoint, with complete declared cross-domain
entry contracts. Calls between mounted domains stay native; writes or remaps
revoke affected bindings before changed code runs.
An explicit `Core::set_memory` invalidates the installed binding even when
addresses are reused. A failed install preserves the previous binding. Installing
code does not clear an architectural fault; recovery is an explicit reset/register
operation.

For joint native composition, `Core::lower` returns owned pre-O3 `SemanticImage`
bitcode, exact guest/event exports and identity. Link both cores and eligible
host implementations before final optimization, resolve every export, then use
`Core::bind` with the exact component symbol map and shared native owner.
Public state/progress/yield definitions join eligible IR; consumers never copy
private layouts or invoke private entry ABIs. Count shared native text once.

Complete known code/variants before execution. Development and deployment may
reuse validated relocatable code artifacts, never device snapshots.
Startup and build-time translation share one semantic pipeline. Release keeps
arithmetic inline and removes proved checks; Debug adds diagnostic instrumentation.
Both retain exact dynamic memory/device/fault behavior. Detailed preparation,
binding and invalidation contracts belong in the target architecture.

## Benchmarks and references

Use independently authored fixtures and equivalent Clang19 O3 C++ references
under the same CPU/features and exact numeric/state contract. Report preparation,
coverage, cold/warm time, code size and API costs separately; verify complete
results and prevent dead-code elimination. checksum/FFT host references and
ASM/SIMD ratios are separate comparisons. JIT/host percent is
100 * host_time / jit_time. Select cases with `CPU12_BENCH_FILTER`; fixtures live
in test/bench/. Model clock ratios are not measured hardware speed, and isolated
core results do not qualify whole-instrument throughput.

Primary references are indexed from [doc/architecture.md](doc/architecture.md).
[License](LICENSE) · [Dependency notices](THIRD_PARTY.txt)
