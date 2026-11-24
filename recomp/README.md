# recomp
The load-time recompiler (M5). Prototype status: see docs/M5_RECOMP.md.

- `gen.py`: player's ROM + `meta/btdd.meta` -> C, one function per routine. Output goes to
  `..\Gamefiles\Movies\m5\generated\` (ROM-derived: never inside this repo).
- `native_rt.h`: helpers the generated code uses (exact interpreter semantics: cycles, bus order, open bus).
- `native_cpu.c`: the run loop with native dispatch by (PRG bank, PC); compiles `src/runtime/cpu.c` into the
  same unit with its `cpu_run` renamed, so the interpreter source is untouched. `BTDD_NATIVE=0` = interpreter only.
- `build_native.sh`: builds `..\Gamefiles\Movies\m5\btdd_native.exe` (headless runner + generated code) with `zig cc`.
- `lockstep.sh`: runs both TAS movies natively and compares RAM every frame with the FCEUX gates.
- `native_jit.c` + `build_jit.sh`: `btdd_jit`, the first-start path: generate (gen.c in-process), compile to a DLL in
  `%LOCALAPPDATA%\btdd\cache` (libtcc in `BTDD_LIBTCC` builds, else the `BTDD_CC` command), load, register.
- `gen.c`: the generator in C (`btdd_gen`, and `btdd_gen_write()` for btdd_jit); output byte-identical to gen.py.
