# M7: mods (2026-11-24)

Mods are our own C code that changes or adds behaviour. A logic mod attaches to the game at ROM routine
addresses; a display mod only changes how the picture is built. Mods never contain translated game code.

## Using them
| mod | key | btdd.cfg | what it does |
|---|---|---|---|
| `nolimit` | F8 | `mod_nolimit = on` | no sprite flicker: every sprite on every line is drawn (the NES shows 8 per line). Picture only |
| `lives` | F9 | `mod_lives = 5` (1-9; `off`) | start, and continue, with N lives instead of 3 |

F8/F9 apply at once and are saved to btdd.cfg. Command lines: `btdd_play --mod nolimit --mod lives=7`, the same
`--mod` for `btdd_headless` and `btdd_shot`. A changed `lives` value takes effect the next time a player starts.

## Cheats: the game's own codes (`src/mods/mod_cheats.c`, settings menu page "Cheats")
| mod | what the game does | how the mod enters it |
|---|---|---|
| `cheat_lives5` | the held code: Up+A+B on the Start press gives that player 5 lives | before bank 6 `$9888` (lives hand-out, Y = player) that player's held buttons read Up+A+B+Start; restored after |
| `cheat_code1` | B, A, Down, B, Up, Down on the screen before stage 1: `$17` = 5, players joining later get 5 lives | before each call of the code checker (bank 6 `$9809`) the new-press byte holds the next button of the sequence (taken from the checker's own position counter); restored after |
| `cheat_code2` | Down, Up, Up, Down, A, B, B, A: `$17` = 10, players joining later get 10 lives | same, sequence 2 |
No lives are written by the mods: the game's own checks run, so the result (including the confirmation sound and
who gets the lives) is exactly the real code's. `mods_test` checks all three (5/5 lives, `$17` = 5 / 10, a second
player joining at frame 1200 gets 10 lives with code 2 and 3 without). Details of the codes: `docs/GAME_FACTS.md`.

## How hooks work (`src/mods/`)
A logic mod lists hooks `{bank, address, kind, fn}` on routine entries:
- **MOD_BEFORE**: `fn` runs when the routine is entered, then the original runs.
- **MOD_AFTER**: when the routine is entered, its return address and stack level are noted; `fn` runs when execution
  reaches that return address with S back at entry + 2 (the routine's RTS), before the caller's next instruction.
- **MOD_REPLACE**: `fn` runs instead of the routine, which then returns to its caller as by RTS, charging the
  hook's stated cycle count.

Mechanism: `Cpu.pc_hook_map` (64 KB, nonzero at hooked addresses) and `Cpu.pc_hook`, checked at instruction start
in the interpreter's `cpu_run`, in the native run loop (`recomp/native_cpu.c`) and in `n_next` (so chained native
code also stops there). Native code hands control back to the run loop at every routine entry and return, so
entry and return-address hooks fire the same way in both. With no logic mod enabled the map pointer is NULL and
emulation is exactly the unmodded one (all gates). `fn` gets the `Nes` (RAM etc.), the `Cpu` (registers) and the
mod's value.

Limits (v1): hooks go on routine entries / return points, not mid-routine addresses (native code runs straight
through those); an after-hook whose routine leaves by unwinding the stack instead of RTS is dropped after 32 newer
ones; pending after-hooks are dropped when a state is loaded or rewound. One console per process. Bank = the
PRG bank mapped when the hook fires (for an after-hook: the bank at entry must also be mapped on return).

Adding a mod: a `mod_<name>.c` with a `ModDef` (id, name, help, kind, value range, hooks), listed in `mods.c`
(`all[]`) and CMake's `btdd_mods`; or `mods_add()` at run time (tests do this). Give it a check in
`tests/mods/mods_test.c`.

## The two mods
**lives** (logic, after-hook on bank 6 `$9888`): that routine gives player Y their starting lives (3, or 5 with a
button code, at least `$0017`; see GAME_FACTS.md) and leaves Y alone; after it returns we store N into
`player_lives,y` (`$0011+Y`).

**nolimit** (display, `src/platform/screen.c`): for each line the screen evaluates all 64 sprites the way the PPU
picks its 8 (at the end of the previous line, same OAM, same 8x16 / pattern-table rules) and draws them all, lowest
OAM index on top. The PPU itself is untouched (it still evaluates 8, sets the overflow flag, does sprite-0 hits),
and so is the game's own per-frame rotation of the sprite order; with all sprites drawn, that rotation only
decides which of two overlapping sprites is on top. Old-PPU model: exact (if the game rewrites a sprite tile
between the PPU's fetch and the end of the line, that line keeps the PPU's 8 sprites: 24 lines in 20,000 frames).
New-PPU model: the PPU's picture already has 8 sprites and no background-transparency bit, so the others are
added where the picture shows the backdrop color (a background pixel of exactly that color counts as transparent).
Not done: undoing the game's rotation for a stable overlap order (needs the game's OAM layout; see M9).

## Checks
- `mods_test` (in `tools/check_all.py` as `mods-test`):
  - lives off: no hook installed, 3/3; `lives=7`: 7/7, the after-hook ran twice (two players);
  - a test before-hook on `$9888` ran twice; a test replace-hook (our code, 6 lives) leaves the run on the same
    course as the after-hook (lives, level, section equal at frame 3000);
  - nolimit, two consoles in lockstep per movie over 20,000 frames: RAM identical on every frame; pictures differ
    only on lines with more than 8 sprites; 2P: 152,475 such lines, 51.6% of frames gain sprite pixels; 1P: 4.7%.
- Native: with `lives=7` the native exe and the interpreter give identical RAM on all 59,351 2P frames (and the
  hook keeps native dispatch on: 40.08M vs 39.98M native instructions in 5,000 frames); mods off, both equal
  FCEUX. In check_all's `native-lockstep` gate.
- btdd_play: `--mod lives=7` over the 2P movie: 7/7 at frame 800; F8/F9 driven in the real window toggle and
  persist to btdd.cfg.
- Every other gate runs with the mod code compiled in and the mods off: unchanged.
- Picture: `btdd_shot --mod nolimit` vs without at 2P frame 16500: a fighter that the 8-per-line limit hides in
  that frame is drawn whole.

Note: `Cpu` gained two fields, so `.snap`/`.state` files saved by older builds are refused (both formats check
the struct layout); re-save them.
