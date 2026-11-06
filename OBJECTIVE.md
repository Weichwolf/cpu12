# Objective

Build a complete, independent, high-performance CPU12/HC12 emulator core with a
native JIT, analogous to the independent dsp56300 library. Execute documented
instructions, flags, addressing, memory transfers, reset, stack/interrupt/wait
behavior and complete readable Core-owned state. Keep CPU semantics separate from
board clocks,
peripheral wiring and application firmware. Make memory/event budgets and code
invalidation explicit, and preserve precise side effects and visible faults.

Native execution must ultimately cover every supported legal instruction and
addressing form. Do not disguise interpreter/helper gaps as completed JIT work.
Keep unsupported host/ISA cases explicit. Specializations need actual guards and
correct side exits. Preserve instrument-visible behavior and musical timing in
the integrating Blofeld Desktop SL emulator: MIDI <-> emulator -> audio.

Use C++23/LLVM19, CMake/Ninja, strict diagnostics and independent directed tests.
Run affected core correctness tests, including independent expectations and
appropriate sanitizer coverage, before publishing source changes. Core tests
remain active before the integrating instrument reaches real time. Keep strict
compilation, formatting and static analysis; never weaken checks.
Keep manufacturer assets and application firmware out of source and tests.
Commit/push verified increments to configured origin/main in this public
repository. Passing an initial captured-instruction subset is not completion.

Keep public integration consistent with dsp56300: explicit typed Memory views,
a Core(Memory, Io*) setup and named captured-instruction execution.
Support common storage ownership with architecture-native units and explicit
aliases, without a virtual memory manager in direct-RAM execution. Emulation is
single-owner; separate host MIDI/audio queues do not change virtual clocks.

Keep the outward public API aligned with dsp56300 and as small as necessary.
Core owns state, exposed read-only when requested; normal consumers do not manage
it. Production execution is JIT-only. Future x86/SSE/AVX and ARM/NEON emitters
implement the same host concept. Preserve the general ISA beyond Blofeld needs.

Before further emulator integration or optimization, both CPU12 and DSP56300
must approach matched host-ASM throughput on representative warmed workloads.
The complete instrument objective remains unchanged. Optimize native blocks,
loops, register/flag liveness and code layout in the cores, validate equivalent
results and report per-workload JIT/host percentages. Passing correctness tests
alone do not establish the performance gate.

Sequence: optimize CPU12 first to >=70% of matched optimized host-ASM throughput
on each representative warm fixture, then optimize and simplify DSP56300 to the
same per-fixture threshold. Until both pass, defer further emulator integration.
Use minimal focused validation during slow-core iteration and broaden it as
performance improves. Keep code small, with complexity justified by correctness
or measured speed. The subsequent complete Blofeld instrument must sustain >=4x
offline real-time throughput on this host under representative high load while
retaining correct guest time and bounded live latency.
