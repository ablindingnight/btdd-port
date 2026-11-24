# M8: save states, rewind, fast-forward (2026-11-24)

## In btdd_play
| key | action |
|---|---|
| F6 / F7 | save / load the current slot |
| 0-9, Shift+F6 / Shift+F7 | pick slot 0-9 / previous / next slot (the overlay says whether it is used) |
| hold Backspace | rewind, one frame back per frame (1x), sound muted; stops at the start of the history |
| hold Tab | fast-forward at `ff_speed` (default 4x) |

Backspace, Tab and the digits act as hotkeys only if the player has not bound them to a game button.
Settings in btdd.cfg: `rewind_seconds` (default 60, 0 = off), `ff_speed` (2-16, default 4).

- Slots are files `<settings folder>\states\<ROM SHA-1>\slotN.state` (the settings folder is the one holding
  the btdd.cfg in use: next to the exe, or `%APPDATA%\btdd`). A save goes to a temporary file first, so a failed
  save never destroys the previous one.
- Rewind history: ~1 MB per second of gameplay (deltas average 16.7 KB per frame on the 2P TAS); the ring is
  capped at `rewind_seconds` x 60 frames and 1.5 MB per second.
- Fast-forward keeps the sound card as the clock: per paced step it runs `ff_speed` frames and queues the sound
  of the last one only, so speed is exact and the sound is a sped-up "skim" instead of silence or a backlog.
- Loading a slot or rewinding clears the queued sound, so no stale audio plays.

## The state (`src/nes/state.c`)
The machine state is the `Nes` struct (CPU, 2 KB RAM, PPU registers, CHR-RAM, nametables, palette, OAM, both
PPU models' internals, APU sequencer and synthesis state incl. filters, mapper bank, controller shift state)
with host fields normalized: function pointers and their users, the debug watch table, the APU's output buffer,
and the picture buffer `fb`, which is only read for the line being drawn and so carries nothing across a frame
boundary. Restoring keeps the receiving host's fields, so a state saved by `btdd_headless` (no APU) loads in
`btdd_play` and the other way round (the APU then keeps its own state if one side had none).

File format (little-endian): 80-byte header, then the normalized `Nes`:
`"BTDDSTAT"`, u32 version (1), u32 sizeof(Nes), u32 layout fingerprint, u32 reserved, u64 frame, u8 PPU model,
u8 APU on, 6 reserved, ROM SHA-1 (40 hex chars), 8 reserved. Loading refuses another version, another struct
layout (any core change that moves fields: re-save) or another ROM. Size: 78.6 KB.

The coverage snapshots (`src/cov/snapshot.c`) use the same normalization helpers; before, a snapshot load or
copy dropped `tick_hook`, the display hook and the APU host.

## Rewind (`src/platform/rewind.c`)
Each frame's blob (state + displayed picture + frame number, 137 KB) is pushed as `prev XOR new`, run-length
coded (zero runs / literal runs, varint lengths). A pop applies the newest delta to `prev`, which gives the
previous blob exactly, so stepping back needs no keyframes and works across loads and resets. The oldest entry
is a delta against a dropped blob, so pops stop one short of it.

## Checks
- `btdd_headless --save-state N FILE` / `--load-state FILE`: saved mid-TAS, loaded, movie continued: 2P (old PPU,
  saved at 30000) and 1P (new PPU, saved at 41234) RAM identical to FCEUX to the last frame.
- `rewind_test` (APU and display attached): 600 pops each identical to a plain copy; restore and replay 600
  frames: state and audio sample-identical; movie continued: RAM identical to FCEUX (2P at 30000, 1P at 41234).
- `btdd_play --movie 2p.fm2 --selftest-states --dump-ram X`: save slot 9 at 3000, rewind 300 frames from 3600
  (RAM == frame 3300's), load slot 9 at 3500 (RAM == frame 3000's), rest of the movie in fast-forward steps:
  RAM identical to FCEUX on all 59,351 frames. The self-test uses `states-selftest\`, never the user's slots.
- Hotkeys driven in the real window by key messages: F6 save, Tab 3 s (frame 838 -> 1562, 4x), Backspace 3 s
  (183 frames back, 1x), F7 load, digit slot pick, empty-slot message.
- `tools/check_all.py` runs all of the above with every other gate.

## Not done
- Slot thumbnails / timestamps in the overlay; save states while a movie plays are not tied to the movie file.
- Rewind speed is fixed at 1x; audio is muted while rewinding (not reversed).
