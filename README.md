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

Declare a `CodeImage` with complete instruction streams, valid entries and
separate source-variant IDs. Keep readable inline data outside instruction
streams; a saved call return address may name data. Supply pointer-free
`VariantLayout` descriptors and real interrupt vector/kind contracts through
`CompileOptions`. `Core::compile(image, options)` owns the input descriptions,
translates every declared instruction start and known variant, and returns a
shareable `NativeImage`. Reject missing lowering, inconsistent code/overlaps,
undeclared encoded targets and incompatible layouts before execution.

Install a concrete prepared variant on its stopped execution owner with
`Core::install(image, variant)`. Validate its actual immutable source bytes and
memory layout, bind instance RAM, and prepare hardware IRQ frames before
publishing the binding. Installation preserves architectural state, musical time
and device state. The Core retains shared code ownership after the caller's
image handle expires.

Require prepared entries for `run`, `step` and explicit instruction execution;
`Fault::unprepared_execution` reports an unknown PC or undeclared event contract
without decoding, emitting or replaying an instruction. An explicit
`Core::set_memory` invalidates the installed binding even when pointers are
reused; install a matching prepared variant before resuming. Reject a failed
installation without changing the previous binding. Register replacement or a
recovered reset clears a latched architectural fault; installing code does not.

Keep mutable data in borrowed host RAM and retain observable program reads and
writes. Select prepared executable variants at real loader/write boundaries.
Fuse compatible consumer-owned host IR before LLVM O3 and preserve actual I/O
ordering. Export linkable bitcode/objects through that same pipeline; keep LLVM
handles private. Exact image/binding identity includes source bytes, ABI, compiler
and native target/features. Compilation and validated artifact loading run before
playback; development always executes the complete compilation pipeline.

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
