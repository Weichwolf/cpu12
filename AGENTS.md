# cpu12

- Build an independent, general CPU12/HC12 native JIT library; keep Weichwolf/cpu12
  private. Complete ISA, flags, addressing, reset, interrupts, waits and checkpoints
  remain the objective. Initial subsets are incomplete. See OBJECTIVE.md.
- Keep board clocks, peripherals and application firmware in consumers. No foreign
  CPU core, Ghidra or Blofeld dependency; use independently authored test programs.
- Use Core(Memory, Io*, Options) with typed borrowed memory, explicit lifetimes,
  aliases, invalidation and bounded execution. The core has one execution owner.
- Native JIT is the default target. Unsupported ISA/host paths fail visibly with
  operation, address and PC; never fabricate retirement or conceal fallback gaps.
- Publish reviewed increments only to configured origin/main; coordinate revision
  changes with the Blofeld consumer. Preserve dsp56300 source attribution.

## Working rules

- Speak German with the user; write code, comments, documentation and commits in English.
- Prefer simple native execution, direct memory and large bounded runs. Keep device
  effects and event boundaries explicit; avoid per-clock work and unnecessary copies.
  Telemetry is runtime-selectable and off by default; keep its work out of hot paths.
- Use C++23, LLVM 19 and CMake/Ninja. Keep strict compiler/linker flags, warnings
  as errors, clang-format, clang-tidy with zero findings and functions <=120 lines.
  Never weaken checks or hide findings with suppressions or diagnostic baselines.
- Release is the performance build. Preserve ASan/UBSan for project code and native
  dependencies, and separate TSan for concurrent code; preserve dependency notices.
- Until the instrument reaches real time, suspend component, functional, regression
  and diagnostic execution tests. Only short optimized startup/throughput probes
  may establish that milestone: stop after at most 10 host seconds, fail unless
  playable, and never extend or automatically repeat a failed probe. Compilation,
  formatting and static analysis remain active. Run fuzzing only on explicit request.
- Afterwards validate affected behavior against independent expectations; avoid
  duplicate or full-suite runs after small changes. Report gaps and measured scope.
- Keep doc/ for one target architecture and important primary references only.
  Requirements belong in OBJECTIVE.md and test/; build metadata belongs in cmake/.
  No activity diaries or historical result manifests. Issue communication belongs
  in private Weichwolf/blofeld-src, with concrete consolidated problems only.
- Keep firmware, samples, manuals, disassemblies and private analysis untracked.
  Preserve source URLs, sizes and hashes in reference manifests; never publish assets.
  Local reference inputs live under ignored build/local/; run artifacts belong
  under ignored build/. Preserve inputs when cleaning generated files.
- Original code is GPL-3.0-only; add matching SPDX identifiers to new source.
- Commit reviewed, verified increments; push only to the configured project remote
  on main. Never publish backup branches or use --all/--mirror. Coordinate core
  revisions with consumer dependency pins; preserve unrelated work.
