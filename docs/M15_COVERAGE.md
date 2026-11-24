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
| after (+ auto-*) | 59,808 (22.8%) | 195,837 (74.7%) | 255,645 (97.5%) |

Per bank, seen before -> after: 0 89.8 -> 97.4%, 1 99.3 -> 99.3%, 2 93.5 -> 98.3%, 3 92.5 -> 97.0%, 4 95.0 -> 96.0%,
5 94.6 -> 97.0%, 6 87.7 -> 95.5%, 7 99.7 -> 99.7%.

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

Random exploration saturates within minutes. Still unseen: 6,499 bytes (92 runs >= 16 bytes = 3,872 bytes,
largest bank 4 $CBF0 528), listed in `..\Gamefiles\Movies\coverage\report\final.txt`; per-source tables in
`report\incremental.txt`.

btdd-decomp frontier (tools/frontier_count.py, same run rules as cdlfrontier/datacarve, read-only; decomp state
of 2026-11-23 ~23:00 with 13,972 raw bytes): old logs 0 carvable executed runs and 0 data tables in raw blocks;
new logs 11 runs (267 bytes; the Qwen loop had already carved the earlier ones) and 156 data tables (5,513
bytes); raw bytes no log has seen 11,995 -> 6,083.

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
Not done: a dedicated character-choice matrix (characters are only varied by exploration bursts from the title
/ menu snapshots), and bots per stage type (random bursts + god mode replaced them).
