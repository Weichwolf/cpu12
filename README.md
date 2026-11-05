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
unary/shift operations, index counters, CCR masks and normal register
transfers/exchanges also execute natively. Indexed/extended byte memory unary
operations add native read/modify/write, write-only CLR and read-only TST.
DBEQ/DBNE, IBEQ/IBNE and TBEQ/TBNE use native counter operations and
conditional transfers while preserving every guest flag. BSET/BCLR and
BRSET/BRCLR add masked byte modifications and memory-conditioned branches.
MOVB/MOVW transfer byte/word data with sequential source/destination index
updates and unchanged flags, using HCS12 next-PC semantics. Native multiply
and guarded unsigned/signed/fractional division preserve documented register
and flag behavior, with indeterminate fields recorded as explicit policy.
ABA/SBA/CBA and documented DAA inputs also execute natively; unqualified DAA
states fail visibly with their original input and PC. IRQ/XIRQ entry, RTI,
WAI and STOP use native vector/stack accesses with precise external faults. No instruction interpreter or
Ghidra dependency is present. Unsupported instructions return an explicit fault
without retirement. This is an initial ISA subset, not a complete CPU12
emulator or a completed Blofeld integration.

The public setup uses `Core(Memory, Io*, Options)`. `Memory::unified(bytes)`
borrows one byte backing; `Memory::window(...)` declares a partial known RAM
window, and `Memory::external()` uses the supplied bus. Memory and peripheral
owners outlive the core. The lightweight callback table is copied at
construction. `Options::cache_entries` bounds native code storage; invalid
bounds fail at setup.

The captured `run_instruction(bytes)` API accepts at most six bytes.
`step()` fetches a complete instruction from current RAM or the explicit
`Io.fetch` callback and executes it natively. `run(instruction_budget)` executes
bounded instruction sequences; it does not supply cycle timing or interrupts.
[Fetched execution](doc/fetch.txt) records exact byte accesses, separate code/data
transports, changed code and fault latching. [Reset](doc/reset.txt) reads the actual
vector; [interrupt entry and return](doc/interrupt.txt) expose eligibility and native
stack transactions. The board owns source arbitration, acknowledgement and time.
[WAI/STOP](doc/wait.txt) report bounded idle exits and reuse the genuine saved
frame on wake. [CPU checkpoints](doc/checkpoints.txt) retain registers, idle
state, IRQ deferral, returned-frame status and faults; callers pair them with board snapshots.
[Post-RTI entry](doc/tail-chain.txt) reuses the just-restored frame for an eligible
pending source before a following instruction retires. The board can query frame
availability separately from source eligibility.
When modeling later RTI refill clocks, `close_return_window()` finishes the board's
pending-source decision without changing guest registers or executing a command.
Remaining ISA, board timing and bounded native regions remain open. The current
RAM/bus contract is described in [memory.txt](doc/memory.txt) and
[indexed.txt](doc/indexed.txt), [control transfers](doc/control.txt) and
[stack/subroutines](doc/stack.txt). [Register/carry ALU](doc/alu.txt) records
flag semantics and the ANDCC/TAP recognition delay.
[Transfers](doc/transfer.txt) cover signed/fixed extensions and register
overlap. [Memory unary operations](doc/memory-unary.txt) describe the real bus
phases and deferred CPU publication. [Loop branches](doc/loop.txt) cover
byte/word counters and signed nine-bit offsets. [Masked memory operations](doc/bits.txt)
cover six-byte captures, address/mask tails and real bus phase behavior.
[Selected-bit reads](doc/masked-read.txt) preserve GPIO bit knowledge and real read
effects without weakening ordinary loads or read-modify-write operations.
[Memory transfers](doc/move.txt) describe operand order and the explicitly
unsupported original M68HC12 move-queue variant. [Multiplication/division](doc/multiply-divide.txt)
describes widths, overflow, zero divisors and the indeterminate-value policy.
[Accumulator arithmetic](doc/accumulator.txt) covers ABA/SBA/CBA, existing
ABX/ABY aliases, decimal adjustment and explicit unqualified-state diagnostics.
See [coverage](doc/coverage.txt) and the [full objective](doc/objective.txt).

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
cmake --build build/release --target format-check
```

Embed with `add_subdirectory(path/to/cpu12)` and link `cpu12::core`. The public
target requires C++23 even when its caller selects an older language standard.
`CPU12_BUILD_TESTING` defaults to ON for standalone builds and OFF when embedded;
both it and `BUILD_TESTING` must be ON to register library tests. The embedded
format target is `cpu12-format-check`, leaving the caller's `format-check` intact.
Diagnostics, clang-tidy, hardening and `CPU12_SANITIZER` remain enabled according
to the selected configuration. Set compatible sanitizer options before adding
the dependency; embedding does not choose the caller's sanitizer policy.

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
