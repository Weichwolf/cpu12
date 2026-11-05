# Target architecture

The library executes the CPU12/HC12 ISA through native JIT code. This design is the
implementation target, not a claim that the current migration is complete.

```text
consumer: memory + peripheral state + virtual time + event arbitration
                            |
                  Core(Memory, Io*)
                            |
               ISA frontend -> host-JIT concept -> native code
                                   |                    |
                        x86/SSE/AVX today        direct RAM access
                        future ARM/NEON          explicit I/O effects
```

The public headers are core.hpp, isa.hpp and memory.hpp. Both libraries expose
the same minimal Core API. ISA-specific register layouts, address units and
peripheral contracts account for their differences; internal JIT mechanics do not.
Core owns its complete state. Normal consumers need no state management; optional
read-only state access occurs at safe owner boundaries and adds no work otherwise.
Checkpoint import/export and restore interfaces do not belong in the library.
State inspection cannot replace or rewind Core state. Staged effect tickets are
outside the normal API.

Only JIT execution belongs in production. Host emitters satisfy one host concept;
there is no interpreter/reference/cached backend selector. Native specialization
must preserve all ISA effects and precise exits. Unknown operations and accesses
fail with operation, address and PC. The general ISA extends beyond Blofeld needs.

Memory uses 16-bit byte addresses and big-endian words. Typed borrowed views
preserve bounds, permissions, knownness, aliases and owner lifetimes. Executable
translations remain immutable for the Core lifetime. Loading a different program
normally creates a new Core. In-system programming is an explicit memory-region
identity transition owned by the consumer: a RAM-resident updater continues with
its existing registers, while changed flash receives a new region identity.
Old translations remain retained; they are not selected for the new identity.
No firmware update protocol, state import or continuous byte hashing is required.
Disjoint RAM grants carry live byte knowledge: unknown loads take an explicit
fault path and native stores mark their bytes known. Read-only program/flash
grants remain distinct from writable RAM and peripheral holes.
Mapping changes occur at retirement boundaries. Validated mapping variants
select retained translations by physical backing identity; they neither rewrite
host code nor require per-instruction program hashes.
One execution owner controls each core; external events use bounded communication.

The consumer owns peripheral clocks and arbitration. Native runs stop at valid
event, interrupt, fault or sleep boundaries; internal grants preserve deadlines
and bounded response. ISA cycle progress and the relative time of real I/O are
execution state, separate from optional benchmark statistics. The consumer adds
derivative memory waits and retains ownership of frequency and virtual time.
A synchronous I/O call may suspend its worker at the actual effect; rejecting
the access remains a fault, never a retry/yield signal.
A due event is handled before executing an instruction
beyond its boundary. Caller-visible instruction counts are optional test control,
not a substitute for event handling. Physical buses and pipeline phases are not
implementation goals. ISA-visible instruction and interrupt semantics remain.

Strict compilation, static analysis and independently expected ISA/state tests
qualify changes. Core tests remain active before instrument real-time operation;
fuzzing runs only on explicit request. Primary references are in reference-manifest.json.

Generated code uses compact shared executable arenas, not one page per operation.
Place frequently connected native blocks together, align entries deliberately,
and keep fault/diagnostic slow paths out of hot instruction streams. Keep active
Core data compact and retain live guest values in host registers across chained
blocks. Qualify locality with code/data working-set and cache measurements; layout
alone does not establish that a whole hot path fits a particular host cache.

I/O rejection contract: a callback returning false is a host diagnostic failure,
not an emulated HC12 bus exception. Interrupt admission publishes registers only
after the vector and complete frame succeed. On rejection, registers retain the
pre-entry values; real external writes already performed remain visible. The
fault latches and later calls cannot replay those writes or vector reads. Tests
exercise refusal at every vector/frame access, including wrapped stack addresses.
This software contract does not assert hardware SP visibility during a bus cycle.

Core stores registers, latched failure, execution state and interrupt-boundary
state together. `state()` returns a stable const reference to that storage for
inspection on the execution owner; native code updates the same fields directly.
The view does not transfer state ownership or provide state import.
