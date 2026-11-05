# cpu12

**An independent CPU12/HC12 native JIT core for fast firmware execution.**

C++23 · LLVM 19 · Linux x86-64 · GPL-3.0-only · Private repository

The library executes guest instructions as native host code. It owns CPU state
and translation; consumers own memory, peripherals, clocks and event arbitration.
It has no dependency on Blofeld, Ghidra or another CPU emulator.

## Status

Native execution covers substantial arithmetic, addressing, control flow, memory,
stack, interrupt and wait behavior. Complete ISA qualification and sustained
instrument performance remain open. Unsupported paths fail explicitly; an early
instruction subset is not a complete CPU12 implementation.

## Build

Requires CMake, Ninja, Clang 19, clang-format-19 and clang-tidy-19.

```sh
cmake --preset release
cmake --build --preset release
cmake --build build/release --target format-check
```

`strict` retains ASan/UBSan. Run affected core tests with `ctest --preset release`
and qualify changed behavior under sanitizers. Core tests remain active while
the integrating instrument is being optimized. Fuzzing requires an explicit request.

## Embed

```cmake
add_subdirectory(path/to/cpu12)
target_link_libraries(your_app PRIVATE cpu12::core)
```

Include `<cpu12/core.hpp>` and construct `Core(Memory, Io*, Options)`.
`Memory::unified`, `window` and `external` describe borrowed storage or callbacks.
Keep backing storage and peripheral owners alive; give each core one execution
owner. Use bounded `run` calls; the consumer supplies time and interrupts.
Program changes must respect native-code invalidation. JIT compilation can
allocate; the core is not an audio callback API. Native code uses W^X protection.

Standalone tests are controlled by `BUILD_TESTING` and `CPU12_BUILD_TESTING`;
embedded builds leave core tests off by default. Match `CPU12_SANITIZER` to the
parent configuration. The embedded format target is `cpu12-format-check`.

## Minimal consumer

This independently authored program executes NOP, then STOP. No peripherals are
needed. The core uses native execution automatically on a supported host.

```cpp
#include <cpu12/core.hpp>
#include <cpu12/isa.hpp>
#include <cpu12/memory.hpp>

#include <algorithm>
#include <array>

int main() {
  std::array<cpu12::Byte, cpu12::address_bytes> ram{};
  constexpr auto program = cpu12::isa::program(cpu12::isa::nop, cpu12::isa::stop);
  std::ranges::copy(program, ram.begin());
  cpu12::Core core(cpu12::Memory::unified(ram));
  const auto result = core.run();
  return result.fault == cpu12::Fault::none ? 0 : 1;
}
```

The backing memory outlives the core. A real consumer supplies its program and
optional I/O, handles sleep/interrupt events and resumes execution. Explicit run
limits remain available for deterministic control; they do not set guest clocks.

## Design and references

- [Target architecture](doc/architecture.md)
- [Complete objective](OBJECTIVE.md) · [Qualification requirements](test/requirements.tsv)
- [Manufacturer reference identities](doc/reference-manifest.json)
- [Development rules](AGENTS.md) · [License](LICENSE)

Manufacturer firmware, manuals and application programs are not distributed.

Native bridge, W^X allocation and build policies retain attribution to our
GPL-3.0-only dsp56300 project in the source.
