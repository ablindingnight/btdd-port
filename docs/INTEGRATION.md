# Integration of the overnight branches (final rehearsal, 2026-11-24)

Branch `integration-rehearsal`, made from `master` (03bb9f0) by merging, in order: `m15-coverage` @ c7fa4b7,
`m2-play` @ 52650bc (includes `m3-apu`), `m6-harness` @ 0c22aa7 (includes `m5-recomp` and `m4-meta`), then one
integration commit. Nothing was pushed; `master` is untouched.

## Conflicts and how they were resolved
| where | kind | resolution |
|---|---|---|
| `README.md` "Targets" paragraph | text conflict (coverage vs play) | one list with every target, followed by play's "Play (M2)" section |
| M5 source lists (`CMakeLists.txt`, `recomp/build_native.sh`) | none this time | M5 now derives them (from `btdd_headless`'s targets in CMake, from the tree's folders in `recomp/core_sources.sh`), so `src/nes/apu.c` and `src/cov/*.c` are picked up without an edit; m6-harness merged with no conflict |
| APU wiring | planned move (docs/M3_APU.md) | `Cpu.tick_hook` called at instruction start after the base cycles (FCEUX's sound-hook point); `Nes` owns an `Apu`, enabled by `nes_enable_apu()` (routes $4000-$4017 writes and $4015 reads, IRQ lines, DMC fetch/stall). Off by default, so `btdd_headless` and the gates run as before. `btdd_apu_check` and `btdd_play` use it instead of the debug exec hook. With `tick_hook` and coverage's `insn_hook` both in `cpu.h`, M5 builds generated code with `BTDD_CPU_HOOKS` and calls both hooks from native code |

## Gates on the merged tree (all identical / passing)
- 2P TAS, old PPU, interpreter: RAM == FCEUX on 59,351/59,351 frames (MSVC, clang and gcc builds)
- 1P TAS, new PPU, interpreter: RAM == FCEUX on 83,576/83,576 frames
- 2P apudiff (APU through `nes_enable_apu`): 59,351/59,351 rows; RAM with the APU attached also identical
- Picture: palette indices == FCEUX at 22/22 frames (2P) and 17/17 frames (1P)
- M5: this tree's `btdd_gen` output is identical to M5's own; native lockstep (zig cc, `BTDD_CPU_HOOKS`,
  `recomp/lockstep.sh` into `..\Gamefiles\Movies\m2\rehearsal\final`): 2P 59,351/59,351, 1P 83,576/83,576
- M5 native + APU: `btdd_apu_check` linked with the generated code: apudiff identical, CSV byte-identical to the
  interpreter build, RAM identical. 120 s WAV byte-identical to M3's when built with `-ffp-contract=off`; with
  zig cc's default it differs in 55 of 5.76M samples by 1 LSB, and so does a zig cc build of the plain
  interpreter (MSVC, clang and gcc builds all equal M3's WAV). Cause: zig contracts the filter math into FMAs
- M6 harness self-test: the passing `sub_8624` candidate passes; the same with one constant changed fails
- nestest 5,003 lines, `cpu_call_test` (6 ok), `apu_synth_test` (7 ok) with all three compilers;
  `release_audit` CLEAN
- Builds: MSVC, clang, gcc, all targets including `btdd_play` and `btdd_jit`. Warnings in our code: one, MSVC
  C4456 in `src/static_main.c:171` (coverage branch; shadowed `k`), harmless

## Follow-ups
- Floating-point contraction: the APU's output filters give different low bits when the compiler fuses
  multiply-adds (zig cc does by default). Add `-ffp-contract=off` for `src/nes/apu.c` (CMake for GNU/Clang,
  `recomp/build_native.sh`, and btdd_jit's DLL compile, though only generated code goes through that), or
  `#pragma STDC FP_CONTRACT OFF` in apu.c, so every build makes the same samples.
- `src/static_main.c:171`: rename the inner `k` (MSVC C4456).
- No script builds `btdd_apu_check` with native code; the rehearsal linked it by hand (core sources minus
  `headless_main.c`, plus `tools/apu_check/apu_check_main.c`). A CMake target would make it a standing gate.
