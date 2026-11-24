# recomp
The load-time recompiler (M5). Prototype status: see docs/M5_RECOMP.md.

- `gen.py`: player's ROM + `meta/btdd.meta` -> C, one function per routine. Output goes to
  `..\Gamefiles\Movies\m5\generated\` (ROM-derived: never inside this repo).
- `native_rt.h`: helpers the generated code uses (exact interpreter semantics: cycles, bus order, open bus).
- `native_cpu.c`: the run loop with native dispatch by (PRG bank, PC); compiles `src/runtime/cpu.c` into the
  same unit with its `cpu_run` renamed, so the interpreter source is untouched. `BTDD_NATIVE=0` = interpreter only.
- `build_native.sh`: builds `..\Gamefiles\Movies\m5\btdd_native.exe` (headless runner + generated code) with `zig cc`.
- `lockstep.sh`: runs both TAS movies natively and compares RAM every frame with the FCEUX gates.
