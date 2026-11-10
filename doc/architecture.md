# Target JIT architecture

Shared design for cpu12 and dsp56300; this is a target, not an implementation claim.

```text
consumer memory + I/O -> Core -> retained native regions and loops
                                  |                     |
                             direct RAM          real event/I/O/fault
```

Core owns complete readable state; consumers own memory and device scheduling.
Only core.hpp and memory.hpp are public. One owner executes each core; different
cores may run independently. No board model, universal core hierarchy, checkpoint
API, effect-ticket API or production interpreter belongs in the library.

Compile from actual code entries along natural control flow. Retain immutable
translations for the core lifetime and link successors natively. No instruction
count caps, budget-specific recompilation, continuous hashing or cache eviction.
Changed executable identities require an explicit owner transition; unrelated
programs use a fresh core.

Provide an explicit synchronous `Core::prepare(established_entries)` before run.
The stopped owner binds an initialized immutable program/mapping revision; the
consumer may transfer that ownership to a preparation worker. Discover code only
from established entries and actual direct edges. Retain unknown indirect edges
and unavailable code as unresolved. Preparation executes no guest instructions,
fetch/read/write/device callbacks or architectural time/state changes. Return
existing rich fault information without latching it into State/Progress. Validate
the captured binding before installing retained native entries.

Lift exact ISA integer, flag, memory and ordered device effects into semantic
LLVM 19 IR. Optimize connected control flow/SSA with target-aware `default<O3>`
and native code generation, keeping LLVM types private. Preparation has no fixed
time budget; substantial cold work is acceptable for better execution. New code
requires a new immutable revision and preparation. Preserve qualified native
execution for unresolved or unmigrated semantics during implementation, without
claiming those paths as completed optimizing preparation. Distinguish discovery,
optimized coverage, code bytes, preparation, first-use misses and warm execution.
Coordinate one process-wide native-target initialization before parallel LLVM
work; separate contexts, modules and executable ownership per prepared core.
Never shut down LLVM while any core retains LLVM objects or code.

Partition prepared code by connected established direct control flow, not length
or execution grants. Keep independent regions in separate functions within one
module/runtime, with region-specific native entry routing. Verify our generated
IR at optimization boundaries; per-pass verification of packaged transforms is
a compiler diagnostic, not production execution work. Extending reachability
retains existing immutable translations and compiles only new entries.

Keep live values in GPR/SIMD registers across instructions and backedges. Preserve
old operands, aliases, flags and ordered effects. Eliminate dead work and hoist
bounds/permission checks only with valid proofs. Exact faults, interrupts and
owner observations publish complete architectural state. Actual I/O calls may
continue natively; generic ISA helper execution remains an explicit coverage gap.

Use compact executable arenas, deliberate layout and cold failure paths. Avoid
per-instruction dispatch, state copies, allocations and telemetry. Translation
boundaries do not depend on execution grants; event handling retains precise
observable ISA prefixes. Consumer buffering and pacing do not change guest time.

Measure realistic continuous workloads against matched optimized host ASM with
identical inputs, relevant semantics and independently checked results. Report
cold translation, warm execution, API/boundary costs, code size and each slow case.
>=70% warm host-ASM throughput is a reference, not an integration gate. Keep
hardware-factor assumptions explicit. SIMD is optional; add complexity only for
correctness or measured speed. Test ISA/flags, RAM/aliases, control, IRQ/wait,
Mandelbrot/FFT and application-appropriate streams. Benchmarks live in test/bench/.

Preserve A/B/D overlaps, big-endian transfers, flags, stack, reset/interrupt
entry, wait states and nominal ISA-cycle progress. Device clocks remain external.

Primary references: reference-manifest.json.
