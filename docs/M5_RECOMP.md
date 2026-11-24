# M5 prototype: ROM + btdd.meta -> native C (2026-11-23)

## Result
- **Both TAS gates are exact with native dispatch on**, also when the C is produced by `btdd_gen` (the C port). CPU RAM equals FCEUX 2.6.6 after every frame of the 2P
  TAS on the old PPU (59,351/59,351 frames) and of the 1P TAS on the new PPU (83,576/83,576), checked with
  `tools/ramdiff.py` against `..\Gamefiles\Movies\m1\fceux_2p.bin` / `fceux_1p.bin`.
- **Share of executed instructions run natively** (default: routines with `conf=verified|static`):
  2P 478.4 M of 536.8 M = **89.1%**, 1P 690.9 M of 733.0 M = **94.3%**.
  With the 66 `suspect` routines compiled too (`gen.py --conf verified,static,suspect`, also exact on both
  movies): 2P **93.6%**, 1P **96.4%**. What is left is almost all code the game runs **from RAM** (an unrolled
  loop it builds at `$0700-$07FF`: 34.0 M instructions in the 2P run); ROM code is 99.97% native.
- **Speed** (machine shared with other jobs; A/B runs interleaved, 20,000 frames of the 2P movie, old PPU):
  interpreter 4,000-4,500 fps, native 6,000-6,900 fps, so the whole emulator is ~1.5x faster. Time spent in the
  CPU drops from 6.6 s to 3.6 s (`BTDD_NATIVE_TIME=1`, timer overhead included in both). The PPU is now most
  of the remaining time. On the new PPU (`cpu_run` is called once per dot, so one native call per instruction)
  both run ~1,200 fps.

## How it works
| file | role |
|---|---|
| `recomp/gen.py` | ROM (SHA-1 checked against the meta) + `meta/btdd.meta` -> `native_b0..7.c` + `native_all.c` in `..\Gamefiles\Movies\m5\generated\` (ROM-derived: never in the repo) |
| `recomp/gen.c` (`btdd_gen`) | the same generator in portable C: byte-identical output to `gen.py` (checked for the default, `--conf verified,static,suspect` and `--exclude-timing --exclude ...`, with clang and MSVC builds); 0.24 s for all 8 banks. This is the step the shipped exe runs at first start |
| `recomp/native_rt.h` | inline helpers used by generated code (bus, stack, flags, ALU), same semantics as `src/runtime/cpu.c` |
| `recomp/native_cpu.c` | run loop with native dispatch; compiles `src/runtime/cpu.c` into the same unit with `cpu_run` renamed, so the interpreter is reused unchanged |
| `recomp/build_native.sh` | `zig cc` build of `..\Gamefiles\Movies\m5\btdd_native.exe` (headless runner + generated code) |
| `recomp/lockstep.sh` | both TAS gates |
| `CMakeLists.txt` | `-DBTDD_NATIVE_GEN=<generated folder>` adds the `btdd_native` target. MSVC (VS 18, Release) builds it in ~30 s with no warnings and passes the 2P gate too |

- **One C function per routine**, `r<bank>_<addr>(Cpu *c)`, entered with `c->pc` at any instruction of the routine
  (a `switch` to its label). Branches and jumps inside the routine are `goto`s. Every instruction is an entry
  point, because the run loop may stop at any instruction boundary (frame budget, interrupt) and come back.
- **Exactness**: each instruction charges its base cycles first, then does the interpreter's bus accesses in
  the same order: page-cross dummy reads and extra cycle, the always-dummy read of indexed stores/RMW, the RMW
  double write, JSR push order, `jmp (ind)` page wrap. Opcode/operand fetches from ROM are skipped; the open-bus
  value they would leave is set as a constant before each data access (controller reads return open-bus bits).
  `p_before` (IRQ polling), `instructions` and `last_op` are kept. RAM accesses go straight to `Nes.ram` (and
  still call the RAM watch hook); everything else goes through the bus callbacks.
- **Budget and interrupts**: before every instruction after the first, native code returns to the run loop if
  the budget is used up or an interrupt is pending, exactly where the interpreter's loop would stop or poll.
- **Control transfers** (JSR, RTS, RTI, `jmp (ptr)`, jumps/branches out of the routine) set `c->pc` and return;
  the run loop looks up `(PRG bank, PC)` in a 8 x 32 KB table of function pointers, else interprets one
  instruction. A store that can switch banks (`>= $4020`, or an indexed/indirect store) returns if the bank
  changed. With clang and `-DBTDD_CHAIN` those returns become guaranteed tail calls into the next native function
  (exact on both movies, but no measurable gain, so off by default and the code stays portable C11).
- **Far calls** (`ldx #lo / ldy #i / jsr $FFCB`) run natively through the trampoline: the stub's bank write,
  `jmp ($13)` and the slot's `jmp` are each native, so cycles and the mapper write stay exact. A literal direct
  C call would skip the stub's mapper write and cycles; it needs the stub's effects inlined first (later).
- **Not native**: routines with `conf=suspect` (default policy; safe to include, see above), `xcode`/unknown
  bytes, code in RAM, `brk` (1 site). Dispatch is by the real PC, so a wrong meta boundary cannot make native code
  run the wrong instructions: it only changes how much runs natively.
- **Timing-sensitive routines are native** because the generated code is cycle-exact; the gates include the
  PCM writer and every sprite-0 poll. `gen.py --exclude-timing` leaves them to the interpreter if a future
  change needs that. The list (from the meta's `timing` records, bank-switch-only routines excluded):
  bank 0 `loc_80ad` `game_start` `sub_855f` `nmi` `sub_fe05` `sub_feda` `sub_ff63` `sub_ffeb`;
  bank 3 `sub_89a9` (PCM); bank 4 `sub_c5eb`; bank 5 `sub_82ba`; bank 6 `sub_8030` `write_vram` `sub_9473`
  `sub_9750` `sub_a3bc` `sub_a3c2` `sub_e4a0` `sub_e622` `sub_f99e` `sub_f9e0`.

## Run it
```
python recomp/gen.py                      # writes ..\Gamefiles\Movies\m5\generated
btdd_gen ROM meta/btdd.meta OUTDIR        # same output from the C generator
sh recomp/build_native.sh                 # zig cc; or CMake with -DBTDD_NATIVE_GEN=... --target btdd_native
sh recomp/lockstep.sh both                # 2P old PPU + 1P new PPU vs the FCEUX dumps
BTDD_NATIVE=0 ...                         # same exe, interpreter only
BTDD_NATIVE_PROFILE=out.txt ...           # interpreted instructions per (bank, PC)
```

## CPU API notes (src/runtime is not edited by M5)
- Missing: a public way to execute one instruction / poll interrupts from another run loop. Worked around by
  compiling `cpu.c` inside `recomp/native_cpu.c` with `cpu_run` renamed. Cleaner: export
  `cpu_execute_one(Cpu*)` and `cpu_poll_interrupts(Cpu*)`, or a `native` lookup hook in `cpu_run`.
- `cpu_call` is not needed by this design (native code never calls the interpreter recursively; it returns to
  the loop), so native and interpreted code can interleave at any instruction.
- `exec_hook` (debug `--exec`) turns native dispatch off for the run.

## Next
1. Embed libtcc: `btdd_gen` already produces the C in-process-ready form (0.24 s); what is left is compiling it with
   libtcc instead of clang/MSVC, caching the result by ROM hash + exe version, and registering the functions.
   With clang -O2 the bank 0 file (3.3 MB of C) takes most of the ~40 s build; tcc will be far faster to compile
   and slower to run, so measure both.
2. RAM code: compile the `$0700-$07FF` routine at run time with invalidation on RAM writes, or keep it interpreted.
3. Speed: check the budget per basic block instead of per instruction (exit points stay exact), drop the
   per-instruction counters in release builds; then the PPU is the bottleneck.
4. The meta gaps listed in docs/M4_META.md (open ends, `xcode`, unknown `jmp (ptr)` targets) only affect how much
   runs natively, not correctness.
