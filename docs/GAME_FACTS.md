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
| **5 starting lives (held code)** | Hold **Up + A + B** while pressing **Start** for the last time before the game starts (the press that leaves the player-select screen). Only that frame counts: holding them afterwards does nothing. Bank 6 `sub_9888` gives 5 lives instead of 3 when that player's held buttons (`joy_held,y`) equal `$D8` (A+B+Start+Up; bits A $80, B $40, Select $20, Start $10, Up $08, Down $04, Left $02, Right $01). Each player has their own: P1 only -> 5/3, P2 only -> 3/5, both -> 5/5. | M7 lives mod, 2026-11-24; **verified** in our core (2P TAS input with the combination added at the Start frame 680; `$0011/$0012` at frame 700) |
| **Code 1: B, A, Down, B, Up, Down** | Enter on **either pad** in the ~1.7 s after the last Start (the screen before stage 1; bank 6 `check_cheat_codes`/`sub_9809` runs on frames 681-785 of the 2P TAS route), at most 60 frames between presses. Sets `$17` = 5 and plays a confirmation sound (APU output changes 2 frames after `$17` is set). Effect: every player who **joins later** (`sub_98ae` -> `sub_989c` -> `sub_9888`: a second player pressing Start in a one-player game, or a continue) gets max(3, `$17`) = **5 lives**. The players already in the game keep the lives they got at the Start press (`sub_9888` runs before the code window). | coverage/M9 session, 2026-11-24; **verified** in our core (scripted input; `$17` = 5 from pad 1 or 2; P2 joining at frame 1200 got 5 lives vs 3 without the code) |
| **Code 2: Down, Up, Up, Down, A, B, B, A** | Same window and rules as code 1; sets `$17` = 10, confirmation sound. Players joining later get **10 lives**. | **verified** in our core (`$17` = 10 from either pad; P2 joining at frame 1200 got 10 lives) |
| Code window has loading pauses | During the code window the game skips its controller read on frames 682-689, 713-718, 737-744 and 760-765 (2P TAS route), so a press that starts and ends inside one is lost (two quick presses of the same button can merge). Press each button for a few frames. | M9 session, 2026-11-24 (watching `$0030` writes) |
| Other effects of `$17` (code active) | Code reading (bank 6), not yet seen in play: `$0682` is set to `$0C` (code 1) / `$0D` (code 2) at the game start (`$97F5` area) instead of 0; `loc_f363` builds `$0C` / `$0E` from it; `sub_f4fe` skips a table check (value `$61`) only with code 2; after the last level (`$10` reaches 7) `$0682` is cleared when a code is active; in level 6 (stage 7) `sub_90a4` makes an extra far call (A = `$28`) when a code is active. Worth a playthrough: possibly a different ending or extra options. | M9 session, 2026-11-24 (code reading, medium) |
| **Cheaters see no ending** | With code 1 or code 2 active (`$17` not 0), beating the final boss does **not** show the ending: when the game moves on after the last stage (level `$07`, the `$9473` branch: `$10` reaches 7 and `$17` is set -> `$0682` = 0), it goes straight back to the title (Tradewest logo) and the attract loop, and clears `$17`. Without a code the same moment leads to the post-boss cutscene, the ending story text (level `$0B`) and the credits. The held 5-lives code (Up+A+B on Start) does not set `$17`, so it keeps the ending. The other `$17` readers (`$0682` = `$0C`/`$0D` at the start, the stage 7 extra far call, `sub_f4fe`) did not change anything visible before this point in the test. | M9 session, 2026-11-24: 2P TAS run to frame 70,000 with `$17` = 5 or 10 poked at frame 56,900 (before the final stage; the end-of-game branch reads only `$17`) vs a normal run; identical until frame 60,763, then title instead of ending. Screenshots `..\Gamefiles\Movies\m9\ending_*` |
| Codes desync the TAS | Entering a code changes RAM from the next frame on (RNG bytes `$2C-$2F` first), so a TAS played with a code active desyncs (the 2P TAS dies and returns to the title near frame 41,000). | M9 session, 2026-11-24 |
| Attract demo | Left at the title with no input: `game_mode` = `$80` at frame 657, a countdown (`$073D`) runs, and at frame 2793 the demo setup stores level 2 (`$942A`), then `$10` cycles through levels 1, 0, 1, 2, 3 (`$F76A`/`$F77A`). A `game_mode` of 3 or more also starts at level 2 (the menu itself only cycles 0-2). | M9 session, 2026-11-24 (headless run without input) |
| Stage 2 background is not its level map | Level 5's camera X runs to about 10,000 px while its block map is 64 blocks (2,048 px) wide, and the nametable matches the map in only 14-18 of 30 rows: the stage builds its background another way (repeating segments?). Stage 7 (level 6) matches only ~70 %. | M9 widescreen, 2026-11-24 (`WIDE_DEBUG`) |
| Camera vs scroll offset | The picture is often scrolled 1-4 pixels away from the camera RAM `$8E:$8D` (stage 1: -1, stage 4: up to +-4): the scroll written in the NMI lags or leads the camera. | M9 widescreen calibration, 2026-11-24 |
| Hidden left column | The game hides the leftmost 8 pixels of the background (PPUMASK) so its column updates are not seen. | M9 widescreen, 2026-11-24 |
| Off-by-one entity Y | In stage 5 section 0B the game's screen Y (`$0494`) of some entity types is one line above depth - altitude - camera Y (the rule `sub_8688` uses elsewhere). | M9 widescreen sprite pass, 2026-11-24 |
| No other input codes | Every compare of `joy_held`/`joy_pressed` with a constant in the carved code was checked: besides the three codes above they are menu and movement masks. | M9 session, 2026-11-24 (decomp scan) |
| Levels 0, 1 and 7 | Valid level IDs that point at dummy data (no hidden levels). | data formats, 2026-11-24 |
| 5 unused songs | Songs `$03`, `$05`, `$0B`, `$11`, `$14` are never requested in the TAS route; forcing them plays them (song `$0B` has a full track). | coverage, 2026-11-24 |
| Unused carry/hold sprite pieces | Bank 6 `$A4F6-$A79D`: extra sprite pieces for ~110 player animations while carrying, never reachable in play. | coverage, 2026-11-24 |
