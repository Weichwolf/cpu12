# Scope

- Speak German with the user; code, comments, documentation and commits are English.
- Build an independent general CPU12/HC12 native JIT emulator core in C++23. The
  private repository is Weichwolf/cpu12; original implementation is GPL-3.0-only.
- Keep board peripherals, manufacturer firmware and application programs outside
  the library. No dependency on the Blofeld emulator, Ghidra or a foreign CPU core.
- Complete documented CPU12 execution, flags, addressing, memory, reset, interrupts,
  waits and checkpoint behavior remain the objective. Early instruction subsets
  and captured-instruction entry points must not be described as complete cores.
- Native JIT is the intended default. Unsupported ISA/host paths fail explicitly;
  never fabricate retirement or silently substitute interpreted execution.
- Keep event and bus contracts precise; unknown accesses include operation,
  address and PC. No firmware patches, skipped routines or successful I/O stubs.
- Use LLVM19, CMake/Ninja, strict warnings as errors, clang-format and clang-tidy
  with zero findings. Functions have at most 120 lines. Do not suppress findings.
- Use the fastest optimized Release for functional work. Until Blofeld firmware
  runs in real time, use focused Release tests and defer extensive sanitizer
  matrices/broad regressions. Preserve ASan/UBSan and separate TSan configurations.
- Run fuzzing only at the user's explicit request; never run it automatically.
- Tests use independently authored instructions and independent architectural
  expectations. Do not copy manufacturer/application firmware or other emulators.
- Preserve reused-source notices. Initial W^X/typed native bridge and build
  policies are adapted from our GPL-3.0-only dsp56300 project.
- Commit verified increments and push reviewed source only to configured
  origin/main. Keep the repository private. No reference assets or backup branches.
- Store local references and run artifacts only under ignored doc/local/ or build/.
