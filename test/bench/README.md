<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Common kernels

Build the Release preset, then run `build/release/cpu12-kernels` (all six),
or select `K1` through `K6`. `--check` checks correctness without requiring
hardware counters and is part of the default CTest suite. Every program uses
`Core::compile`, `install`, `run` and, for K6, `run_until` and `interrupt`.
There is no benchmark-only execution path or algorithm replacement in the core.

The guest programs are independently encoded HCS12 instructions. The separately
compiled portable C++ in `reference.cpp` uses Clang O3 with native tuning and
without LTO, so the caller cannot remove work after seeing its expected result.
All expected outputs are checked outside the measured interval. Inputs, tables,
coefficients and references require no network, manuals or sibling repository.

| Kernel | Work | Complete result check |
|---|---|---|
| K1 | 64-tap causal FIR, 256 blocks of 1024 samples | All 262,144 Q1.15 samples and contract SHA-256; exact instruction/base-cycle counts |
| K2 | Four transposed DFII biquads, same blocks/input | All samples and eight final states; both contract SHA-256 values |
| K3 | 256-point radix-2 complex FFT, 512 transforms | All 512 signed component words after every transform; independent instruction count |
| K4 | 1,048,576 table-selected transitions, nested calls | Entire RAM, visible registers, exact instructions/base cycles |
| K5 | Copy 4096 16-bit words with a running checksum, 512 passes | Entire RAM, visible registers, exact instructions/base cycles |
| K6 | K1 with a bound every 256 base cycles and IRQ every 4096 | K1 output/hash, 87,747 IRQs, exact instructions/base cycles and 32-bit guest counter |

K1/K2/K6 implement the definition in `doc/core-contract.md`, section 8. The
shared reference generator remains in dsp56300; it is neither copied nor
compiled here. The independent portable implementation and the guest programs
are bound to the contract's Q1.15 full-vector hashes, serialized as sign-extended
signed 32-bit little-endian words. The LCG advances once per chronological
sample; FIR history starts at zero and crosses every block boundary. Biquad
state also persists across blocks. The guest uses `EMACS` with a 32-bit memory
accumulator. The common quantizer adds a parity-dependent bias through carry,
clamps, then extracts the signed result. The C++ rounds the magnitude instead.
The coefficients bound every unquantized intermediate to signed 32 bits;
intermediate saturation never replaces cancellation.

K3 receives the fixed complex input in bit-reversed order, as does its C++
comparison. Natural-order real/imaginary input i is `((i*97+13)&4095)-2048` /
`((i*53+7)&4095)-2048`. Twiddles are exp(-2*pi*i*k/256) rounded at scale 32768
and symmetrically clipped to [-32767,32767], allowing signed negation on HCS12.
Each complex product sum is quantized with the same nearest-even Q1.15 rule;
each butterfly result is halved with floor rounding. Small inputs and per-stage
scaling preserve headroom. HCS12 has no bit-reversed address mode or FFT hardware;
guest address arithmetic and three nested counted loops implement the transform.
The six-byte native table holds real, imaginary and negated imaginary words;
the independent C++ reads only the canonical real/imaginary table.

K4 uses native 16-bit words and eight prepared handler addresses. Starting at
state 3 and checksum 0xace0, a transition adds state, XORs state+1, calls a common
subroutine that adds 3, returns through both stack frames, then advances state
by 3 modulo 8. D and A/B overlap normally. All arithmetic wraps at 16 bits.
K5 likewise uses the processor's natural 16-bit word/checksum rather than a
wider core's word: source word i is `(i*0x9e37) xor rotl16(i+0x123,i mod 16)`.

K1/K2 stage one block at a time in consumer storage; K3 stages one transform.
Staging and complete output verification are outside both native and portable
measurement intervals. K6 charges all owner scheduling and IRQ delivery inside
those intervals. WAI terminates a complete block/program, including its real
stack frame and nominal cycles. No guest instruction is replayed at a bound.

The CSV reports a warm median of three runs after warm-up: user-mode host
instructions and cycles per guest instruction, guest instructions/s, native/C++
elapsed-time ratio (greater than one is slower), standalone preparation seconds
and native text bytes. Linux uses one perf_event_open group on the execution
owner thread, excluding kernel/hypervisor work and scaling multiplexed counters.
Counter refusal is an error. macOS runs correctness checks; counter collection
there remains unqualified. Pin the benchmark to one host CPU and run measurements
without concurrent own builds or native workloads.

CoreMark 1.0 remains an external reference point, not one of these kernels;
no CoreMark result is currently qualified. Manufacturer/compiler inputs belong
under ignored `build/local/`. Core measurements do not establish consumer audio
throughput or live latency.
