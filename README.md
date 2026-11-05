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

`strict` retains ASan/UBSan. Until the integrating instrument reaches real time,
execution tests are suspended; only optimized startup/throughput probes of at
most 10 host seconds are allowed. Fuzzing requires an explicit request.

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

## Design and references

- [Target architecture](doc/architecture.md)
- [Complete objective](OBJECTIVE.md) · [Qualification requirements](test/requirements.tsv)
- [Manufacturer reference identities](doc/reference-manifest.json)
- [Development rules](AGENTS.md) · [License](LICENSE)

Manufacturer firmware, manuals and application programs are not distributed.

Native bridge, W^X allocation and build policies retain attribution to our
GPL-3.0-only dsp56300 project in the source.
