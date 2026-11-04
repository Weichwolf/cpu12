# cpu12

Independent CPU12/HC12 native JIT emulator core, written in C++23. GPL-3.0-only.

The initial implementation compiles captured NOP, 8-bit immediate load/ALU and
short conditional-branch instructions directly into x86-64/Linux host code.
Direct/extended byte and word load, ALU and store instructions use native RAM
access or explicit external bus callbacks. Word operations cover D, X, Y and SP. Native entry requires the detected LAHF
long-mode CPU feature; unsupported hosts refuse explicitly.
Arithmetic, CCR changes and branch decisions execute natively. No instruction
interpreter or Ghidra dependency is present. Unsupported instructions return an
explicit fault without retirement. This is an initial ISA subset, not a complete
CPU12 emulator or a completed Blofeld integration.

The captured instruction API separates CPU semantics from the caller's fetch,
clocks and board model. Interrupt entry, reset and waits remain required work,
along with indexed addressing and bounded native regions. The current RAM/bus
contract is described in [memory.txt](doc/memory.txt). See
[coverage](doc/coverage.txt) and the [full objective](doc/objective.txt).

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --build build/release --target format-check
```

LLVM19 and Ninja are required. Release uses O3, native host tuning and ThinLTO.
The strict preset retains ASan/UBSan; broad sanitizer work is deferred until the
integrating Blofeld firmware reaches real time. Fuzzing is never automatic.
Executable code is emitted into writable pages and then protected read/execute;
no page is simultaneously writable and executable. Compilation/cache misses can
allocate or throw setup errors; the current entry point is not a real-time audio
callback contract. Warm register/RAM execution has no ISA helper calls. External
memory accesses
call the supplied bus only; arithmetic and flags still execute natively.

The native bridge, W^X allocation pattern and build policies are adapted from
our independent [dsp56300](https://github.com/Weichwolf/dsp56300) library under
GPL-3.0-only. Manufacturer firmware, manuals and private analysis are not shipped.
