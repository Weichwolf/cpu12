# Target architecture

Core repositories are organizational boundaries. Choose APIs, state ownership,
layouts and LLVM composition for complete-instrument throughput and efficiency.
Core owns complete readable state; consumers bind memory, firmware, device clocks
and scheduling through minimal public APIs. Each core has one
execution owner; independent cores communicate through consumer-owned contracts.
Keep LLVM handles, native entry ABIs and implementation layouts private.

## Complete preparation and composition

```text
CodeImage + CompileOptions -> Core::lower -> SemanticImage
                                                |
other core IR + eligible host IR ---------------+
                                                v
                          native target + joint LLVM19 optimization
                                                |
                              shared code owner + export map
                                                |
                      Core::bind -> NativeImage -> stopped install
```

Declare every established executable stream/start, source variant and event
contract separately from readable data. A saved return address need not be code.
Validate complete instruction extents, operands and coherent overlapping sources.
Include known indirect targets, loader outputs, IRQ vectors and retained fetch
forms. Unknown dynamic transfers may select only prepared targets. Incomplete
coverage fails with identity, address and reason before execution.

Core::lower owns source/variant/layout metadata and detached pre-O3 IR without a
runtime Core, borrowed execution RAM, TargetMachine or native engine. Preserve
all guest/event roots. Compile exact ISA semantics once; consumers never copy
lowering or reconstruct private core state. Core::compile uses the same
lower/materialize/bind path for standalone composition.

Link eligible host implementations before final optimization. Validate target,
layout, features, scalar/aggregate types, calling convention, ABI attributes,
noexcept and parameter/memory guarantees before linker attribute merging.
Callbacks use public Io roles and per-instance owners; they may observe public
Core state or request synchronous yield. Do not assume callbacks cannot alias
Core state. Missing selected definitions fail materialization, not lowering.
Retain unselected roles' normal owned I/O semantics.

Core::bind requires exact identity, every declared export once, nonnull addresses
and shared executable ownership. Reject missing/extra/duplicate symbols. Copy
typed maps; borrowed symbol names/compiler inputs may expire after binding.
Initialize/finalize the shared native owner once and count native text once,
not per component/variant. Consumers never invoke private guest entry ABIs.

Install only on a stopped owner after validating actual source, complete layout
and the relevant revision. Installation emits no guest/event code and changes no
architectural state, memory or time. Rejected installation preserves the previous
binding and pending source-version handoff. Fault recovery is an explicit
register/reset boundary, not a side effect of installing code.

Declared cross-version transfers return the completed prefix without replaying
the jump. Retain registers, stacks, captured events and nominal progress; validate
and install a matching declared prepared destination. A source/entry may name
several finite destinations, checked against actual immutable code and memory
layout; duplicate triples remain errors. Pending choices borrow immutable image
metadata under retained ownership, including across remaps and rejected installs.
Remaps or source changes revoke dependent callers/targets. Unrelated RAM must not
force retranslation of independent code. Unknown entries and undeclared events fail before new decoding
or emission; no production interpreter or ordinary-emitter coverage fallback.

## Native code and validation

Build natural connected CFG/SSA regions, functions and loops, independent of
quotas and catalogue entry density. Preserve all declared starts without turning
each into a function boundary. Link known successors directly, use prepared
indirect targets and keep the host stack bounded across cycles. Retain immutable
translations and live values across native edges; no length caps, budget-specific
recompilation, continuous hashing or hot-path allocations/telemetry.

Represent exact integer/flag, memory and ordered device effects in IR. Apply
constant/range propagation, CSE, dead-store elimination, forwarding, register
propagation and legal loop/vector optimization. Arithmetic lowers inline to
native integer operations; opaque per-opcode helpers are not optimized native
coverage. Materialize state only at required observable/fault/IRQ/partial-grant
boundaries. Preserve old parallel operands, aliases and precise prefixes.

Prove code/layout/rights and storage/knowledge-alias invariants during AOT or
stopped binding. Revalidate relevant revisions, then expose valid contracts to
LLVM. Fully initialized stable RAM uses direct accesses without repeated word
knowledge checks. Removing a check requires a proof covering invalidating writes,
remaps and legal inputs; a code hash or warm trace alone is insufficient.
OS protection does not establish guest mapping, initialization or MMIO semantics.
Do not infer outermost-allocation noalias guarantees merely from range separation.

Keep unavoidable dynamic address/device/fault selection and cold exits precise.
Real device operations can continue natively; never repeat a committed effect
after a guard fails. Run and event handling use exact grants without arbitrary
instruction slices or changed guest time. Consumer horizons and source timestamps
remain separate from local nominal-cycle bounds.

Generated code follows the host build mode. Debug includes assertions and selected
diagnostic/sanitizer instrumentation. Release uses joint LLVM19 O3 with no optional
Debug/sanitizer work or redundant proved checks. Both preserve the same required
ISA/fault/device semantics. Verify source/IR/bindings during preparation; inspect
optimized code to qualify the execution path. Instrument our host/generated code,
not packaged foreign libraries. Sanitizer checks execute at runtime after insertion.

## Artifacts and ownership

Prefer startup loading/full AOT before playback; support build-time IR/object
export through the same frontend and code-quality contract. Preparation is
side-effect-free and has no fixed time budget. Complete native initialization
before execution. Initialize LLVM once per process before parallel preparation;
never shut it down while another image owns code.

Development always runs full AOT. Operational artifacts include code/variants,
exact core/LLVM pipeline, binding ABI, host target/features and build mode.
Resolve relocations safely; never serialize instance pointers or device snapshots.
Reject stale/corrupt entries and rebuild before execution. Manufacturer inputs
and derived artifacts stay outside tracked source/tests.

## Qualification

Compare authored guest algorithms translated through LLVM19 O3 against equivalent
Clang19 O3 C++ on the same CPU/features, with exact numeric/state/memory semantics
and independently checked outputs. Prevent dead-code elimination. Report coverage,
cold compilation, warm throughput, code size, API costs and every slow case.
Retain ASM/SIMD comparisons separately; >=70% matched host-ASM is an additional
reference. Model clock ratios do not establish measured silicon speed.
The consumer's >=10x instrument target requires actual original audio;
microbenchmarks do not qualify it.

## CPU12 contracts

Preserve A/B/D overlaps, big-endian transfers, flags, stack, reset, interrupt
entry, WAI/STOP/RTI and nominal ISA cycles. Device clocks remain consumer-owned.
Compile IRQ kind/vector frame construction, waiting/returned-frame entries and
STOP wake with each variant. Use the same memory ordering as ordinary execution.

Explicit memory remaps invalidate the installed binding even when pointers are
reused. Prepare owner-specific entries/frames before playback; variant selection
must not emit glue. Validate direct/indirect callees transitively before reuse.

Foreground and asynchronous-entry bounds are separate. Prove actual vector
grants, stack writes and masks, including XIRQ nesting inside IRQ. Preserve
WAI/RTI frame reuse, deferrals, return windows and exact entry costs. Tracked-RAM
stores initialize saved bytes without reading old values. Split/unknown/mutable
vectors cannot establish the same direct proof. Stack/mask changes, unresolved
flow and possible owner effects end the supplied bound; it grants no permission
to pass a possible input. Consumers combine channel knowledge and device clocks.

## References

Primary input identities are in [reference-manifest.json](reference-manifest.json).
N64Recomp, XenonRecomp and Remill inform static translation/overlays, native
integration and exact LLVM semantics; they are references, not runtime dependencies
or evidence of this core's coverage/performance.
