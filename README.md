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

`Core::prepare(established_entries)` analyzes the bound immutable instruction view
without executing guest instructions or changing State/Progress. It compiles
supported operations through target-aware LLVM O3 and eagerly installs retained
native entries. Current lowering covers immediate loads into A/B/D/X/Y/SP,
immediate ADDD/SUBD/EORA, INX/DEX/INY/DEY, NOP and branches;
unmigrated instructions and unknown edges remain explicit and use existing native
execution. No public LLVM or backend selection is exposed.
Disconnected control-flow regions use separate native functions in one module;
connected loops remain whole. IR is verified before and after optimization.

The report counts prepared and unresolved guest entry addresses, newly emitted
native text bytes, and non-latching preparation diagnostics. Repeated preparation
reuses code and translates only newly established entries. New memory identities
retain independent executable lifetimes.
Call only on the inactive execution owner; rebinding and real I/O remain the
consumer's responsibility.

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

Memory belongs to the consumer and must outlive Core. One thread owns execution;
state inspection is optional. Use Io for device effects and run_until() for an
absolute local cycle deadline. Core does not schedule an instrument or its audio.
Unsupported operations fail explicitly.

Benchmarks check independent results and report cold/warm timing and matched
JIT/host ratios (`100 * host_time / jit_time`). All scenarios have x86-64 and
ARM64 host references. Hardware factors use stated models, not measured device
speed. Warm populations use a fixed 100 ms warm-up after cold measurement;
packet warm-up advances both engines continuously. Select cases with
CPU12_BENCH_FILTER; fixtures are in test/bench/.
See doc/architecture.md and test/ for details.

[License](LICENSE)

LLVM dependency notices are retained in [THIRD_PARTY.txt](THIRD_PARTY.txt).
