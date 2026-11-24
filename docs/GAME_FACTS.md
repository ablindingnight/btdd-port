# Game facts measured for the port (addresses and values only, no ROM bytes)

## Level and area variables (measured 2026-11-23 from the 2P TAS per-frame RAM log
`Gamefiles\Movies\musiclog\full\ram.bin`, 59,351 frames; verify with the M1 core before relying on them)

**`$000D` = current level ID.** Settles at each stage start; briefly `$FE` during area transitions, `$FD` at
some transitions (title->game at frame 657, and once in stage 4 at 40496).

| TAS stage (order played) | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| level ID in `$000D` | 3 | 5 | 2 | 4 | 8 | 9 | 6 |
| starts at frame | 813 | 7347 | 19231 | 35740 | 42856 | 53322 | 56961 |

IDs **0, 1 and 7 never appear in the 2P TAS**: candidates for unseen content (intro/ending/bonus/cut level), a
first target for the coverage engine's stage warp.

**`$0010`** steps through intermediate values during each transition and then equals the level ID (03, 05, 02,
04, 08, 09, 06). Probably "level being loaded / load step". The decomp's proposal `lives_or_score` for `$0010` in
`btdd-decomp/docs/RAM_PROPOSALS.md` is likely wrong.

**`$0015` = area/section ID (global numbering).** Changes at every section transition inside a level (the same
frames where `$000D` blips to `$FE`), and resets to 0 during stage-end transitions:

| stage | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| sections seen (`$0015`) | 04, 05 | 08, 09, 0A | 01, 02, 03 | 06, 07 | 0B, 0C | 0D | 0E |

By level ID: level 2 = sections 1-3, level 3 = 4-5, level 4 = 6-7, level 5 = 8-A, level 8 = B-C, level 9 = D,
level 6 = E (so mostly in level-ID order, except level 6). Section 0 and anything above 0E are unseen in the TAS.
Use: gate detection (a section change = a gate passed) and warp targets for the coverage engine.

## Already known (from btdd-decomp/src/ram.inc)
- `$0011/$0012` lives P1/P2 (start 3), `$0016` game_mode (1 = 2P mode A friendly fire, 2 = mode B),
  `$051B,x` ent_hp (slot 0/1 = players), `$0200-$02FF` OAM buffer.
