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

## Lives (M7, 2026-11-24, watched on the 2P TAS)
- Bank 6 `$9237` (sub_9237) sets both lives to 3 when the title/menu sets up (2P TAS frame 670; `$0012 = $FF` in
  1P mode), then copies them, xor `$80`, to `$05F2/$05F3` for the HUD.
- Bank 6 `$9888` (sub_9888) gives player Y their starting lives when the game starts (frame 690, Y = 0 then 1):
  3, or 5 if that player's held buttons (`joy_held,y`) equal `$D8`, but never less than `$0017`. Y is unchanged on
  return. The `lives` mod hooks here (after-hook).
- An extra life is a plain increment elsewhere (P2 at frame 1152, bank 0 `$91E2`).

## Secrets, cheats and quirks (add every one you find, with how it was found)
| What | How it works | Found |
|---|---|---|
| **5 starting lives** | Hold **Up + A + B** and press **Start** when a player's game starts. Bank 6 `sub_9888` gives 5 lives instead of 3 when that player's held buttons (`joy_held,y`) equal `$D8`. Button bits: A $80, B $40, Select $20, Start $10, Up $08, Down $04, Left $02, Right $01 (ram.inc: Start = $10, Down = $04, A = $80), so $D8 = A+B+Start+Up. | M7 lives mod, 2026-11-24 (code reading; verify in play) |
| Levels 0, 1 and 7 | Valid level IDs that point at dummy data (no hidden levels). | data formats, 2026-11-24 |
| 5 unused songs | Songs `$03`, `$05`, `$0B`, `$11`, `$14` are never requested in the TAS route; forcing them plays them (song `$0B` has a full track). | coverage, 2026-11-24 |
| Unused carry/hold sprite pieces | Bank 6 `$A4F6-$A79D`: extra sprite pieces for ~110 player animations while carrying, never reachable in play. | coverage, 2026-11-24 |
