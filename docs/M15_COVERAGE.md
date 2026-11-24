# M1.5: coverage engine (2026-11-23)

Our headless core now does the logging that FCEUX/Mesen sessions did: code/data logs, a data-reader table,
snapshots along the TAS, stage warp, god mode, a coverage-guided explorer and a static completion probe.
All outputs are ROM-derived and live under `..\Gamefiles\` (never in this repo); the logs that btdd-decomp
should merge are published as `..\Gamefiles\Debugger\auto-*.cdl` (its `tools/cdl.py` merges every .cdl there).

## Tools
| Tool | What it does |
|---|---|
| `btdd_headless --cdl X --cdl-mesen Y --reads Z` | Code/data log in FCEUX format (FCEUX 2.6 rules: one effective address per instruction, quarter bits, indirect bits, vectors; `--cdl-fceux-clear` also clears a byte on a write like FCEUX), the same run in Mesen format with jump-target and subroutine-entry bits, and the PRG data-read table (byte, reader PC, innermost routine from a shadow call stack, `$000D`, `$0015`, count) |
| `btdd_headless --snap-dir D` | Snapshot every 60 frames (`--snap-every`) and at every `$000D`/`$0015` change and camera lock/unlock; gate log `gates.txt` / `gates.tsv` / `gate-*.ram` |
| `btdd_headless --load S.snap` | Continue from a snapshot (the movie continues at the snapshot's frame) |
| `btdd_headless --warp L[:S] --god --poke A=V@F[+] --movie-until N --press-every N P1[,P2]` | Stage warp, god mode, RAM pokes, scripted presses (src/cov/play.h) |
| `btdd_headless --shot-every N DIR` + `tools/shots.py DIR` | Pictures (background + sprites) as PNG |
| `btdd_explore` + `tools/explore.py` | Coverage-guided random play from snapshots, one process per core |
| `tools/forced_paths.py` | Attract mode, Start-only play (1P, game over, continue), 2P game over, mode B, ending, 1P TAS, koh, feos, lamp and sub635 movies |
| `tools/fm3tofm2.py`, `tools/fcm2fm2.py` | TAS Editor (.fm3, binary) and old FCEU (.fcm v2) movies to text .fm2 |
| `btdd_static` | Static completion probe: never-executed branch/JSR/JMP targets of executed code, decoded and probed |
| `tools/covreport.py` | Coverage per bank and per source, still-unseen runs |

Snapshots are the `Nes` struct with pointers cleared: they belong to one build of the core (the header holds
the struct size and a layout fingerprint; loading refuses a mismatch). Re-record them after a core change.

## Recorder validation (2P TAS)
- RAM still identical to FCEUX old PPU on all 59,351 frames with the hooks installed.
- With `--cdl-fceux-clear`, our log is a strict subset of `fceux-tas.cdl` and every common byte has identical
  flag bits (code/data, quarter, indirect code, indirect data).
- `fceux-tas.cdl` has 2,739 more code bytes and 5,810 more data bytes. Running 140,000 frames past the movie
  end (ending, credits, title) explains 1,084 code bytes; the remaining 1,655 code bytes (bank 6 $F0xx-$FExx,
  bank 2 $F3E0-$F451, bank 3 sub_b320) are all in the 11-21 hand-play Mesen logs (btdd.cdl: 1,641 of them) and
  never ran in our replay. Because our RAM matches FCEUX on every frame, the TAS cannot have run them: FCEUX's
  logger session for that file was not started fresh (it also logged hand play).
- Without `--cdl-fceux-clear`, 43 extra data bytes stay flagged (bank-switch writes FCEUX would clear).

## Snapshots and gates (2P TAS)
1,279 snapshots (every 60 frames + 27 level changes, 21 section changes, 69 camera unlocks and the matching
locks); loading any of them and replaying the rest gives RAM identical to the straight run. Camera position
is `$8E:$8D` (X) and `$90:$8F` (Y). No single "wave cleared" counter explains every unlock: several unlocks
follow the enemy HP slots `$051B,x` (x >= 2) dropping to 0 (frames 4782, 5927, 6502), others do not; `$32/$33`
drop to 0 before 54/51 of the 69 unlocks but change ~1,100 times per stage (input/animation state, not a gate).
Exploration starts from these snapshots, so gates the TAS passed are already open.

## Level and section IDs (stage warp)
- Stage start = bank 0 `$8312` (`sta $10; sta $0D`, A = level). `$0010` is the level the logic uses (compared with
  4, 7, 8, 9, $0B, $0D in bank 6 `set_scroll_and_ppu_frame`); between stages it counts stages (0 at the title,
  1 after stage 1, ...) and bank 6 `$9736` turns it into the next level through an order table at bank 6 `$903A`:
  3, 5, 2, 4, 8, 9, 6, then `$FF` (end). Bank 6 `sub_a0fb` sets `$0015` from a per-level first-section table
  (bank 6 `$A0E2`) only when `$0015` is 0.
- **Level 0** is the title/menu value of `$000D` (seen in the TAS from power-on to frame 657), and **level 1** is
  the inter-stage counter value of `$0010`: neither is a stage. Warping to them jumps into RAM garbage and jams
  the CPU within 3 / 112 frames.
- **Level 7** has first section 0 in the table; warping there (and to $0B, $0C, $0D) hangs in the NMI's sprite-0
  wait (bank 0 `$8180`/`$8190`, no sprite 0 is ever placed): not startable as a stage. Level `$0A` runs but shows
  stage-7 section data with the wrong tiles. None of them looks like hidden content; the order table never
  produces them.
- **Section 0** is what level 7 gets (see above). Section table entries exist up to `$10` (`$11` = end marker):
  `$0F` runs (wrong tiles from level 6; level 9 and 3 move on to their own sections), `$10` and `$11` jam.
- Warp logs added 51 code bytes, mostly from runs that later jammed (garbage), so they are **not published**;
  they stay in `..\Gamefiles\Movies\coverage\warp\` with pictures.

## Results (2026-11-23, all logs in ..\Gamefiles\Debugger)
| | code bytes | data-only | seen |
|---|---|---|---|
| before (13 existing logs) | 56,168 (21.4%) | 190,302 (72.6%) | 246,470 (94.0%) |
| after (+ auto-*), 2026-11-23 evening | 59,808 (22.8%) | 195,837 (74.7%) | 255,645 (97.5%) |
| after the night run, 2026-11-24 02:35 | 60,032 (22.9%) | 195,943 (74.7%) | 255,975 (97.6%) |

Per bank, seen before -> evening -> night: 0 89.8 -> 97.4 -> 97.5%, 1 99.3 -> 99.3 -> 99.3%, 2 93.5 -> 98.3 -> 98.4%,
3 92.5 -> 97.0 -> 97.2%, 4 95.0 -> 96.0 -> 96.0%, 5 94.6 -> 97.0 -> 97.1%, 6 87.7 -> 95.5 -> 96.1%, 7 99.7 -> 99.7 -> 99.7%.
The night gain (+224 code, +330 seen) is all from the static pointer-table / jump-list rule (probe3); exploration
round g added 0 code and 73 data-only bytes.

New code / data-only bytes of each source compared with the 13 old logs (sources overlap: exploration round A
found most of what feos-2p and lamp have before those movies arrived):
- explore-a 2,363 / 5,027; later rounds b, c, d add 64, 2 and 31 code bytes no other log has.
- feos-2p (2011 2P route that uses deaths) 853 / 403; its truncated version adds nothing more.
- lamp (evgs TAS Editor glitch route, converted with tools/fm3tofm2.py; reaches level 9 by frame 1,275, then
  runs bank 6 sub_f4fe / sub_f7bc / $F363) 447 / 231, 8 code bytes only it has.
- static probe 727 code (109 candidates, 15 rejected by decoding, 94 clean; the cpu_call probe almost always
  returns, so decoding is the real filter; fall-throughs of always-taken branches are skipped).
- sub635 (2005 1P FCM, converted with tools/fcm2fm2.py; stage 1 and part of stage 2 on the new PPU, then it
  desyncs) 15 / 72; tas1p-oldppu 5 / 6.
- No new code: 2P TAS, 1P TAS (new PPU), koh movies, attract, Start-only 1P, Select+Start, 2P game over, mode B,
  ending (the hand-play/FCEUX logs already had them). sub553 (2005 1P FCM) desyncs on the title screen on both
  PPU models and is not used.

Random exploration saturates within minutes. Still unseen: 6,169 bytes after the night run (6,499 before it;
38 gaps >= 32 bytes = 2,630 bytes in `report\gaps.txt`, largest bank 4 $CBF0 528), listed in `..\Gamefiles\Movies\coverage\report\final.txt`; per-source tables in
`report\incremental.txt`.

btdd-decomp frontier (tools/frontier_count.py, same run rules as cdlfrontier/datacarve, read-only; decomp state
of 2026-11-23 ~23:00 with 13,972 raw bytes): old logs 0 carvable executed runs and 0 data tables in raw blocks;
new logs 11 runs (267 bytes; the Qwen loop had already carved the earlier ones) and 156 data tables (5,513
bytes); raw bytes no log has seen 11,995 -> 6,083. Night run, decomp state of 2026-11-24 02:35 (593 raw blocks, 8,073
bytes): all logs give 0 carvable executed runs, 4 data tables (48 bytes); 5,861 raw bytes no log has seen.

## Rerun everything
```
cd btdd-port-cov   (or btdd-port after the merge); build: cmake --build build-clang
set G=..\Gamefiles  &  set C=%G%\Movies\coverage
btdd_headless ROM --movie %G%\Movies\tasvideos-4323M-2p.fm2 --cdl %G%\Debugger\auto-tas2p.cdl --cdl-mesen %G%\Debugger\auto-tas2p-m.cdl --reads %G%\Debugger\auto-reads-tas2p.tsv --ram-pcs %C%\tas2p-ram-pcs.txt
btdd_headless ROM --movie %G%\Movies\tasvideos-4323M-2p.fm2 --snap-dir %C%\snap-tas2p
python tools/forced_paths.py --out %C%\forced --publish
(movies-ram-pcs.txt = sorted union of --ram-pcs from the 2P TAS, the 1P TAS --ppu new, koh-btdd --frames 38594)
python tools/explore.py --snaps %C%\snap-tas2p --out %C%\explore-a --seconds 600 --seed 11 --publish explore-a
python tools/explore.py --snaps %C%\snap-tas2p,%C%\forced\snap-* (comma list) --out %C%\explore-c --seconds 600 --seed 33 --publish explore-c --extra "--burst 200,1500 --god-ratio 0.7"
btdd_static ROM --known KNOWN.txt (list of Debugger\*.cdl) --snaps SNAPS.txt (one snapshot per stage) --ram-pcs %C%\movies-ram-pcs.txt --out %C%\static\probe   then copy probe-m.cdl to Debugger\auto-static-m.cdl
python tools/covreport.py --new "auto-*" --regions
python tools/frontier_count.py %C%\baseline %G%\Debugger
btdd_headless ROM --movie ... --movie-until 800 --frames 2400 --warp 7 --god --shot-every 200 DIR ; python tools/shots.py DIR
```
Bots per stage type were not written (random bursts + god mode replaced them).

## Night run 2026-11-24 (indirect targets, gap hunting)
**Indirect-target log** (`btdd_headless --indirect`, always on in `btdd_explore`; merged with
`tools/indirect_merge.py` into `..\Gamefiles\Movies\coverage\indirect_targets.tsv`: kind, site bank/PC,
target bank/PC, for far calls the slot's own JMP target, count, sources). Far calls are logged when execution
enters the `$FFCB`/`$FFCD` trampoline: site = the JSR/JMP before it, target = bank (`$FFB3,y`) and slot `$14:X`.
- The 8 unknown `jmp (ptr)` sites of M4: 7 got targets. Bank 0 `$FDFD` -> `$8397`, `$D241`; bank 3 `$89A6` ->
  `$89A9-$89AC` (4); bank 5 `$80B8` -> 18 targets `$812A-$81AA` (step 4) and `$8127` -> 28 targets `$8132-$81AA`;
  bank 6 `$80AA` -> `$80AD-$80CE` (step 3) and `$810A`; `$9992` -> `$9A8E`, `$9A94`; `$9995` -> `$8EFD`, `$8F0C`,
  `$E75E`. Bank 0 `$FE20` (`jmp ($2A)` in the other NMI path) never ran in any log; the only values the game ever
  stores in `$2A/$2B` (bank 0 `$85D1/$85D3`) are `$D241` and `$8397`, so those are its likely targets.
  **Forced (2026-11-24):** `sub_fe05` is entered from the bank 0 NMI only when `$09` bit 7 is clear, and reaches
  `$FE20` only when it is set, so `$FE20` runs only when an NMI arrives through another bank's NMI stub
  (`$FF9C`-style: switch to bank 0, `jmp $FE05`) while the main loop is idle; that never happened in any log.
  With bank 0 `$FDE2` patched in memory to `lda #0` (`btdd_headless --prg-poke 0:FDE2=A9,0:FDE3=00`, every NMI
  then takes this path) the game keeps running and `$FE20` jumps to `$8397` (19,020x) and `$D241` (745x) in
  20,000 frames: the same two targets as `$FDFD`. Confidence: high for the target set (RAM differs from the
  normal run only by timing: the random generator first, then the TAS desyncs).
- The 6 dynamic far calls: all resolved. Bank 0 `$84C4` -> bank 3 slots `$800C-$8015`; `$9F6C` -> bank 5 `$801E`,
  `$8021`; bank 2 `$FD49` -> bank 4 `$8015`, `$8027`; bank 4 `$ABB9` -> bank 3 `$8003-$8015`; `$C599` -> bank 1
  `$8003`, bank 7 `$8003`; bank 6 `$E7ED` -> bank 3 `$800C`, `$800F`, `$8015` (slot JMP targets in the TSV).

**Gap hunting.** Exploration rounds: e (title/menu/transition snapshots, single-button menu taps, 9M frames):
0 new code; f (stage 2 sections 08/09, stage 5 0B/0C, stage 6 0D; 13M frames): 0 new code; g (every snapshot
source incl. forced 1P / mode B states in stages 2-7, 2.5 h, cut to 2 workers for machine load, 58M frames):
0 new code, 73 data-only bytes. Forcing 1P
(`$16`=0) or mode B (`$16`=2) at the start of stages 2-7, all 25 character pairs (`$05F0/$05F1`) through the stage
1 -> 2 intermission, and 1P / other characters / the `$06A6` and `$0683` branches of the ending choice (bank 6
`sub_f3ca`) added nothing either: the ending is always level `$0B` (reached via `$10` = 7 at bank 6 `$9731`).
The static probe now also takes never-called slots of jump lists and the targets of never-read entries of
pointer tables (e.g. bank 6 `$997D` table: entry `$9986` -> `$9B52`, 96 bytes; `$9C58` -> `$9E3E`, 47 bytes):
106 clean candidates, 951 bytes.

## Human-play checklist (Mesen, code/data logger on, save as a NEW .cdl in Gamefiles\Debugger)
Checkpoint states for every stage/section (FCEUX .fcs, no movie attached) and a README with the steps are in
`..\Gamefiles\SaveStates\checkpoints\` (made by `tools/fceux/checkpoints.lua` + `tools/fcs_checkpoints.py`;
Mesen cannot load .fcs, so use FCEUX's Debug > Code/Data Logger with them).
What automation cannot reach is mostly data selected by game situations (`tools/gap_context.py` lists every gap
with its readers and the stage/section where neighbouring bytes are read). Already covered automatically, no
need to repeat: title/menus/attract, every character pair, 1P and mode B starts, pause, game over/continue in 1P
and 2P, the ending and credits. In rough order of bytes at stake:
1. **Stage 2 (Colossus interior, level 5), sections 08 and 09, and stage 6 (level 9) section 0D, 1P, play slowly**:
   let every enemy wave spawn, wait at each scroll lock until enemies stop coming, walk back left, get hit by
   every enemy type, use every move (bank 6 tables `$A552-$A7xx`, `$CAA9`, `$CC2E` read by bank 6 `$8C15`; ~330 B).
2. **Stage 5 (level 8), sections 0B and 0C**: same, plus lose a life there and continue (bank 3 `$CE93`, `$E7BD`,
   `$DD30` read by bank 3 `$8CFB`/`$D73F`; bank 5 `$C80F`; ~230 B).
3. **Stage 3 (level 2), sections 01-03**: slow play, all enemies (bank 5 `$E41A`, `$E29A` read by bank 5 `$B151`;
   bank 3 `$DC02`; ~140 B).
4. **Between stages**: reach each stage normally (not by a TAS route) in 1P and 2P and watch every intermission
   and stage intro to the end without pressing anything (bank 2 `$F7CC-$F93F` read at section 00 of levels 2, 5,
   8, 9, 6; bank 4 `$BDE6`, `$BEC5`, `$C043-$C1FC` metasprites read by bank 4 `load_metasprite_c6c5` / `$C7B0`
   during `$FD/$FF` transitions; ~600 B). If an intermission differs between 1P and 2P or after a death, that is
   the case to log. **Done by script (2026-11-24, `Debugger\agent-intermissions.cdl`)**: the TAS up to every stage
   end (2P and 1P), then no input for 4,000-8,000 frames: **+1,147 bytes** (bank 4 545, bank 2 370, bank 6 232;
   all data, no code). Forced 1P (`$16`=0) and both players on their last life gave nothing more. Scripted idle,
   walk-left, slow-right, punch/jump, up/down and Start-every-300-frames play from all 20 section starts
   (`agent-checklist-play.cdl`, 120 runs) gave nothing new, so items 1-3 and 5 really need varied human play.
   Seen after this: 98.1% (5,022 bytes unseen, 30 gaps >= 32 bytes = 2,000 bytes).
   **Varied play by script (round h, 2026-11-24)**: `btdd_explore` (held directions, attack/jump rhythms, idle
   pads, pause) from 2,668 fresh gameplay snapshots of every stage (2P TAS, 1P TAS old PPU, forced 1P stages
   4-7), god mode off, 20% deliberate deaths, 4 cores x 1 h = 34M frames: **+2 bytes**. Random play is
   exhausted. **Forced songs** (`agent-songs-forced.cdl`: stage 1's song slot bank 0 `$838C` poked in memory to
   the songs the TAS never requests, `$03 $05 $0B $11 $14`): +93 bytes of bank 3 song headers/pointers and song
   `$0B`'s stream. Unseen now: 4,929 bytes. What is left in items 1-3 is specific: e.g. bank 6 `$A4F6-$A79D`
   (the overlay table that `sub_8c15` draws on top of a **player** frame only while `$04B2,x` bit 5 is set,
   indexed by the player animation `$03D1,x` * 4): ~110 player animations never happened in that state. Finding
   what sets bit 5 (an item/hold/ride state in stages 2 and 6) is the next lead for a human or a targeted script.
5. **Stage 4 (level 4) section 06 and stage 1 (level 3) sections 04/05**: slow play (bank 5 `$FF32`, bank 3
   `$9C69`, bank 4 `$B8B6`).
6. **Title text, bank 4 `$CBF0-$CDFF` (528 B) and `$CE0C`**: drawn by bank 4 `sub_c5fe`/`sub_c61a` (screen text);
   every menu path was tried automatically, so this is probably unused text (or a screen behind an input code).
   Only worth a try if someone knows of a secret code or alternate screen.
