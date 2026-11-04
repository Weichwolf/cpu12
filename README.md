# cpu12

Independent CPU12/HC12 native JIT emulator core, written in C++23.
GPL-3.0-only.

The initial implementation compiles captured NOP, 8-bit immediate load/ALU and
short/long conditional branches and JMP directly into x86-64/Linux host code.
Direct/extended byte and word load, ALU and store instructions use native RAM
access or explicit external bus callbacks. Indexed forms add dynamic X/Y/SP/PC,
constant/accumulator offsets and indirect pointer accesses. Word operations
cover D, X, Y and SP. Native entry requires the detected long-mode LAHF CPU
feature; unsupported hosts refuse explicitly. Arithmetic, CCR changes and
branch decisions execute natively. Stack save/restore, JSR/BSR and RTS also
compile to native memory and control transfers. Carry arithmetic, inherent
unary/shift operations, index counters and CCR masks also execute natively. No instruction interpreter or
Ghidra dependency is present. Unsupported instructions return an explicit fault
without retirement. This is an initial ISA subset, not a complete CPU12
emulator or a completed Blofeld integration.

The public setup uses `Core(Memory, Io*, Options)`. `Memory::unified(bytes)`
borrows one byte backing; `Memory::window(...)` declares a partial known RAM
window, and `Memory::external()` uses the supplied bus. Memory and peripheral
owners outlive the core. The lightweight callback table is copied at
construction. `Options::cache_entries` bounds native code storage; invalid
bounds fail at setup.

The captured `run_instruction(bytes)` API separates CPU semantics from the
caller's fetch, clocks and board model. Interrupt entry, reset and waits remain
required work, along with remaining ISA and bounded native regions. The current
RAM/bus contract is described in [memory.txt](doc/memory.txt) and
[indexed.txt](doc/indexed.txt), [control transfers](doc/control.txt) and
[stack/subroutines](doc/stack.txt). [Register/carry ALU](doc/alu.txt) records
flag semantics and the remaining interrupt-timing requirement. See
[coverage](doc/coverage.txt) and the [full objective](doc/objective.txt).

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --build build/release --target format-check
```

LLVM19 and Ninja are required. Release uses O3, native host tuning and ThinLTO.
The strict preset retains ASan/UBSan; broad sanitizer work is deferred until
the integrating Blofeld firmware reaches real time. Fuzzing is never automatic.
Executable code is emitted into writable pages and then protected read/execute;
no page is simultaneously writable and executable. Compilation/cache misses can
allocate or throw setup errors; the current entry point is not a real-time
audio callback contract. Warm register/RAM execution has no ISA helper calls.
External memory accesses call the supplied bus only; arithmetic and flags still
execute natively.

The native bridge, W^X allocation pattern and build policies are adapted from
our independent [dsp56300](https://github.com/Weichwolf/dsp56300) library under
GPL-3.0-only. Manufacturer firmware, manuals and private analysis are not
shipped.
