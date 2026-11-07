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

Prioritize the complete Blofeld instrument and connected firmware execution.
Fix and optimize CPU12 or DSP56300 when integration exposes a generic core defect
or a measured throughput or latency bottleneck. Keep the cores independent and
generally usable; manufacturer firmware remains outside their source and tests.

Retain >=70% of matched optimized host-ASM throughput on representative warm
workloads as an optimization reference, not a prerequisite for emulator
integration. Instrument correctness, measured whole-device throughput and live
latency determine application progress. Compare identical inputs and relevant
ISA semantics with independently verified results; report every slow scenario
and first-use deadline miss. Tiny microfixtures and averages cannot establish
sustained performance. Use scalar or SIMD lowering according to measured benefit;
SIMD is optional. No current performance or completion is implied.

Provide a `bench` command under the tests that reports assumed hardware throughput
(target >=1x) and warm JIT/host throughput, with clock/cost assumptions and the
comparison scope. Cold runtime compilation must also fit actual processing packet
deadlines: report the worst cold and warm packets, first-use translations and
missed deadlines separately from average throughput. A cold microkernel time alone
is not a processing deadline. Add a persistent cache only if measurements justify
it as necessary to avoid first-use underruns.

Use focused validation during slow-core iteration and broaden it as performance
improves. Keep code small, with complexity justified by correctness or measured
speed. Complete Blofeld integration must sustain >=4x offline real-time
throughput on this host under representative high load while retaining correct
guest time and bounded live latency.
