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
| `tools/forced_paths.py` | Attract mode, Start-only play (1P, game over, continue), 2P game over, mode B, ending, 1P TAS, koh movies |
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
