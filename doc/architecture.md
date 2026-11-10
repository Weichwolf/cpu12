# Target Ahead-of-Time Binary Translation architecture

Shared design for cpu12 and dsp56300.

```text
consumer memory + I/O -> Core -> retained native regions and loops
                                  |                     |
                             direct RAM          real event/I/O/fault
```

Core owns complete readable state; consumers own memory and device scheduling.
Only core.hpp and memory.hpp are public. One owner executes each core; different
cores may run independently. No board model, universal core hierarchy, checkpoint
API, effect-ticket API or production interpreter belongs in the library.

The consumer orchestrates Ahead-of-Time Binary Translation of complete known
executable images and variants. Core libraries
provide semantic ISA translation; they own no firmware or host device lifecycle.
Prefer startup firmware loading and full AOT before playback; preserve the same
native code-quality contract as build-time compilation. Startup/compiler memory
is separate from warm throughput; no lazy translation during playing qualifies.
Mandatory preparation may generate a local build-time translation compiled/linked
with the application, or execute before run. Fuse compatible concrete host IR with
ISA IR before LLVM 19 O3. The result runs as native host code over borrowed host
RAM; preserve program words where code itself observes or changes them.

Complete coverage gates execution for each validated code/binding version.
Translate every established executable function, interrupt/indirect entry and
known variant before playback; a partial optimizing subset is insufficient.
An incomplete translation reports identity, address and reason. Known loader
output and instruction templates select already prepared native variants while
genuine program writes retain their effects. Do not assume arbitrary new code
during playing without evidence. Runtime data remains mutable host RAM.
Keep generated firmware artifacts untracked; reject mismatched images, ABI or
host features.

One semantic frontend and optimization pipeline serves both materialization paths:
install native code in the stopped startup owner, or export linkable LLVM bitcode
or relocatable native objects for build-time AOT. Core projects must provide this
capability; a consumer must not copy ISA lowering internals. Export artifacts carry
code/variant hashes, compiler/target/features and binding ABI identity. Resolve
memory/context/device symbols by linking or validated relocation, never serialized
process pointers. Keep LLVM ownership private; expose only the minimal consumer
artifact/binding contract. Shared IR is optimized together before host lowering.
Treat translated code and concrete host functions as one optimized native path.
Inline stable bindings and remove avoidable call/dispatch boundaries, state
copies, spills and repeated validation. Qualify generated code and connected
profiles while preserving every required memory/device effect.

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
requires a new immutable revision and preparation. Resolve executable frontiers before complete preparation succeeds. Distinguish discovery,
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
Lower complete acyclic single-instruction regions directly through the qualified
native emitter; there is no inter-instruction work to optimize. Natural cycles,
including one-opcode cycles, retain optimizing loop lowering.
Rebinding unrelated RAM must not retranslate code whose complete instruction
region and compiled binding requirements remain unchanged. Captured RAM pointers
require matching grants; parameterized functions use the current context.

Keep live values in GPR/SIMD registers across instructions and backedges. Preserve
old operands, aliases, flags and ordered effects. Eliminate dead work and hoist
bounds/permission checks only with valid proofs. Exact faults, interrupts and
owner observations publish complete architectural state. Actual I/O calls may
continue natively; generic ISA helper execution remains an explicit coverage gap.

Use compact executable arenas, deliberate layout and cold failure paths. Avoid
per-instruction dispatch, state copies, allocations and telemetry. Translation
boundaries do not depend on execution grants; event handling retains precise
observable ISA prefixes. Consumer buffering and pacing do not change guest time.

Measure realistic continuous workloads against equivalent portable C++ compiled
by Clang 19 O3 for the same host CPU/features, targeting throughput parity.
Match inputs, fixed-width arithmetic, flags/state and observable memory effects;
check results independently and prevent dead-code elimination. Report actual
guest lowering, complete preparation coverage and remaining boundary costs.
An ordinary native-emitter run is not an LLVM-O3/AOT result. Retain matched
ASM/SIMD comparisons and existing gates as separate additional references. Report
cold translation, warm execution, API/boundary costs, code size and each slow case.
>=70% warm host-ASM throughput is a reference, not an integration gate. Keep
hardware-factor assumptions explicit. SIMD is optional; add complexity only for
correctness or measured speed. Test ISA/flags, RAM/aliases, control, IRQ/wait,
Mandelbrot/FFT and application-appropriate streams. Benchmarks live in test/bench/.

Preserve A/B/D overlaps, big-endian transfers, flags, stack, reset/interrupt
entry, wait states and nominal ISA-cycle progress. Device clocks remain external.

Binary-translation precedents: N64Recomp (native C functions and known overlays),
XenonRecomp (C++ plus a consumer runtime) and Remill (C++ ISA semantics lifted to
LLVM with consumer-owned memory bindings). These are design references, not
dependencies or evidence of this core's coverage/performance.

Primary references: reference-manifest.json.
