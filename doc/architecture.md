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
Development always exercises the full AOT pipeline. Provide persistent validated
artifacts for deployment, preserving exact code/variant, core/LLVM pipeline, host
features and binding identity. Rebuild rejected artifacts before execution.
Design generated code and host integration for the consumer's 10x throughput target.
Treat translated code and concrete host functions as one optimized native path.
Inline stable bindings and remove avoidable call/dispatch boundaries, state
copies, spills and repeated validation. Qualify generated code and connected
profiles while preserving every required memory/device effect.

Compile from actual code entries along natural control flow. Retain immutable
translations for the core lifetime and link successors natively. No instruction
count caps, budget-specific recompilation, continuous hashing or cache eviction.
Changed executable identities require an explicit owner transition; unrelated
programs use a fresh core.

Lower an explicit `CodeImage` through owner-free `Core::lower(image, options)`.
Declare all instruction streams and valid entries per source variant, separately
from readable inline data. A call's saved return address need not be executable.
Own source bytes, instruction boundaries, layout descriptors and interrupt
contracts before returning the immutable shareable `SemanticImage`. Lower every
declared start, including starts not reachable from the supplied root entries;
reject inconsistent overlaps, missing operands, undeclared encoded targets and
missing semantic lowering. Pointer-free layouts describe each variant's memory
requirements; concrete RAM addresses belong only to execution owners.
Compile the declared IRQ vector/kind push, waiting and returned-frame entries
plus STOP wake in the same LLVM module as their variant. Reuse the ordinary
memory lowering and frame-store order; load actual storage through the owner's
runtime bindings. Link eligible host IR before final O3 for these entries too.

```text
CodeImage + CompileOptions -> Core::lower -> SemanticImage
                                                |
other core IR + concrete host IR ----------------+
                                                v
                          consumer native target + joint LLVM O3
                                                |
                            one object/native owner + export map
                                                |
                 Core::bind(semantic, NativeSymbols) -> NativeImage
```

`SemanticImage::bitcode()` owns detached LLVM 19 IR before final O3.
`exports()` provides owned name/ABI views for every guest and event root;
`identity()` is a content digest. Include original source bytes, entries,
variants, layouts, transitions, IRQ and callback contracts, exact core source,
LLVM version, target/features, embedded runtime IR and semantic IR in artifact
identity. Use an image-derived namespace for roots. Distinct images with equal
variant IDs must compose without symbol collisions. No runtime Core, borrowed
RAM, device owner, native TargetMachine, object or execution engine belongs in
lowering. Default core-owned I/O adapters stay embedded.

The consumer materializer imports both core artifacts and eligible host IR into
one native compilation. Validate targets/layouts/features and incoming callback
definitions before linker attribute merging. Preserve every declared guest and
event root through final O3 and relocation. Resolve every export before
publication; initialize/finalize the shared native resource once. Guest and
event entry ABI descriptions identify relocation contracts; consumers never
invoke those private core entry signatures directly.

Export ABI text starts with `llvm19;cc=<numeric calling convention>;type=<LLVM
function type>`. Append `;<index>:<LLVM attribute>` for ABI attributes on the
return value (index zero) and parameters (indices starting at one), in this
order: signext, zeroext, inreg, sret, byval, byref, inalloca, preallocated, nest,
swiftself, swifterror, swiftasync, returned. Optimization-inferred attributes are
not ABI identity. Treat descriptors as versioned opaque contracts; validate
the actual type, calling convention and ABI attributes before and after O3.

Pass `NativeSymbols{owner, symbols, identity, native_bytes}` to `Core::bind`.
Supply the exact component identity and complete component-specific export view;
reject empty owners, wrong identities, missing/extra/duplicate names and null
addresses. Copy typed entry maps and immutable source/layout/event metadata;
retain the same `shared_ptr<const void>` owner in every bound component.
Binding creates no native compiler or Core and borrows no export-name storage.
Semantic/compiler artifacts and original symbol tables may expire after binding.
`native_bytes` describes the entire shared owner's text resource. Count it once
for an instrument, never once per root, variant or bound core.

`CompileOptions::callbacks` selects public I/O roles by external symbol name.
Derive selected declarations from actual Clang-compiled adapter calls. Keep
unselected roles on their runtime route. Callback definitions may access public
Core state, request synchronous yield and use per-instance `Io::owner`; do not
assume owner/state disjointness. Preserve exact scalar types, calling convention,
ABI attributes, noexcept and argument guarantees. Reject stronger parameter
preconditions before importing definitions. Missing selected definitions fail
materialization, not lowering.

`Core::compile(image, options)` uses the same lower, standalone materialize and
bind phases. `CompileOptions::bindings` supplies standalone host bitcode only;
lowering neither parses nor captures it. Standalone bindings contain no absolute
host pointers, mutable global state or implicit global initialization. Instance
state arrives through runtime owners. Use the consumer materializer for the
joint instrument compilation. Rebuild consumers against matching headers and
libraries when the public value/API layout changes.

Install a selected variant with `Core::install(image, variant)` on its stopped
owner. Validate original immutable source bytes and the actual complete memory
layout, build the native entry index, and bind the already compiled event entries.
Installation generates no machine code. Publish only the complete successful
binding and retain shared code ownership. Installation executes no guest instructions or device
callbacks and changes no architectural state, memory or time. A rejected install
preserves the complete previous binding. Fault recovery remains an explicit
register/reset boundary.

Declare finite source-version handoffs with `CompileOptions::transitions`, using
`CodeTransition{from, to, entry}`. The source must not contain that instruction
entry; the target must name it as an established entry. Reject unknown identities,
self transfers and ambiguous declarations before lowering. An encoded absent
successor requires such a declaration; unknown dynamic targets remain errors.

After actual instruction or accepted IRQ execution reaches a declared target,
return the completed prefix without fault and retain the required image/variant.
The current register PC identifies the destination; subsequent execution retires
nothing until the same image's target is installed. Preserve the pending identity
across remaps and rejected installations, including source/layout failures.
Register replacement, captured execution, preparation and new IRQ admission must
not bypass this boundary. Reset cancels it through the real reset transaction.
Selection preserves stack/flags, WAI state, RTI return windows, fresh interrupt
deferrals and nominal progress. Never replay the loader or its final transfer.

Installed execution uses prepared native entries exclusively through run, step
and explicit instruction APIs. An unknown PC or undeclared hardware event fails
with `unprepared_execution`, PC and address before fetch, instruction emission or
new event code. An explicit memory remap invalidates the installed binding even
when addresses are reused. Select and install the matching already prepared
source version before resuming; source writes and variant transitions retain
all original observable effects. Mutable data remains owner-bound RAM.
Prepare every required per-owner variant binding before playback. Runtime
variant selection must reuse its prepared entry/frame bindings; it must not
emit event glue or guest code on the first loader, bank or template transition.

Lift exact ISA integer, flag, memory and ordered device effects into semantic
LLVM 19 IR. Optimize connected control flow/SSA with target-aware `default<O3>`
and native code generation, keeping LLVM types private. Preparation has no fixed
time budget; substantial cold work is acceptable for better execution. Resolve
executable frontiers before complete preparation succeeds. Distinguish discovery,
optimized coverage, code bytes, preparation, first-use misses and warm execution.
Coordinate one process-wide native-target initialization before parallel LLVM
work; immutable images may serve independent execution owners. Keep source IR,
compiler state, executable ownership and per-instance mutable data distinct.
Never shut down LLVM while any image retains LLVM objects or code.

Partition prepared code at natural call entries and control-flow boundaries,
not length or execution grants. Keep distinct call entries in native functions
within one module/runtime. Preserve every edge inside a function or as a direct
native transfer; optimize all eligible units and bindings together with O3.
Native transfers retain cumulative progress and the last completed operation,
including when the next entry reaches its quota/deadline before retiring work.
Indirect transfers select only prepared native targets. Keep the host stack
bounded over cycles and preserve original observable stack reads/writes;
compiler helpers may consume return addresses as data. Verify our generated
IR at optimization boundaries; per-pass verification of packaged transforms is
a compiler diagnostic, not production execution work. Extending reachability
retains existing immutable translations and compiles only new entries.
Lower every declared guest instruction through the shared semantic pipeline;
an ordinary emitter is not a fallback for missing AOT lowering. Hardware event
frame bindings are cold owner work and preserve their precise native contracts.
Natural cycles, including one-opcode cycles, retain optimizing loop lowering.
Rebinding unrelated RAM must not retranslate code whose complete instruction
and binding dependencies remain unchanged. Direct native callees and prepared
indirect targets are transitive code dependencies: changing or losing their
source/binding invalidates a retained caller too. Keep genuinely independent
code reusable. Captured RAM pointers
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

Derive foreground and interrupt-entry time bounds separately. A pure foreground
cycle does not exclude asynchronous output. For an IRQ entry, verify the actual
immutable vector word grant, target coverage, stack writes and mask state; include
an eligible XIRQ nested inside an ordinary IRQ. Preserve WAI/RTI frame reuse and
exact entry costs. Direct tracked-RAM stores initialize saved bytes without
reading their old values or consulting the owner. Split, unknown or mutable
vector grants cannot establish the same direct entry proof. End the continuation
proof at stack/mask changes, unresolved control flow or possible owner calls.
An entry bound applies to its supplied state and binding; it establishes neither
source assertion time nor permission to run past a possible input. The consumer
combines source-channel knowledge, local device deadlines and clock segments.
Closing an RTI return window changes the entry contract before ordinary execution.

Binary-translation precedents: N64Recomp (native C functions and known overlays),
XenonRecomp (C++ plus a consumer runtime) and Remill (C++ ISA semantics lifted to
LLVM with consumer-owned memory bindings). These are design references, not
dependencies or evidence of this core's coverage/performance.

Primary references: reference-manifest.json.
