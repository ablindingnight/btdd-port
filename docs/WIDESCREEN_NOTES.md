# Widescreen notes (M9), 2026-11-24

What a wider-than-256-pixel view needs, from the camera, scroll, gate, spawn and collision facts in
`docs/DATA_FORMATS.md`. Addresses only. Confidence as in that file.

## 1. Why the PPU picture cannot simply be widened
- AxROM has **one-screen mirroring**: the game has a single 256x240 nametable. The renderer (bank 5 `loc_a004`)
  writes the column (or row) that is about to scroll into view and overwrites the one that scrolled out. There
  are no off-screen columns to show (high).
- So the extra columns must be **drawn by the port from the level data**: block map (`lvl_map`), block set
  (`lvl_blocks`), attribute byte per block (`lvl_block_attr`), using the current CHR-RAM and palette RAM
  (`$5B-$7A`). All three are located per level by bank 5 `$A4A1-$A4EE`; block address = map + row * `$BE` +
  column, with column = world X / 32 (DATA_FORMATS sections 1-2). This is enough to draw any 32x32 cell of the
  current level at any world position (high: format confirmed by poke tests).
- CHR is uploaded per section (bank 1/4/5/7 lists) and the player tiles are streamed every frame into the sprite
  half; background tiles stay put within a section, so extra columns drawn with the live CHR-RAM are correct
  (medium: check animated background tiles, if any, per stage).

## 2. Per-stage view limits
| stage (level) | map width | widescreen |
|---|---|---|
| 1 (3) | 72 blocks (2304 px) | yes; deck edges (collision segments) are in the map |
| 2 (5) | 64 blocks; camera `$8E` stops at `$11` | yes, clamp to the map end |
| 3 (2) | **8 blocks = 256 px**, vertical | no extra columns exist: pillarbox (or draw a border) |
| 4 (4) | map in RAM `$0600` (built by `sub_a452`, a 36-byte strip) | the playfield is generated; extra columns would be empty space: draw starfield only, or pillarbox |
| 5 (8) | 144 blocks; `$EA` sub-areas switch to other block sets (`$EA + 9` index) | yes, but follow `$EA` |
| 6 (9) | 32 blocks | yes |
| 7 (6) | 16 blocks (512 px) | little room: at most 256 extra pixels in total |
Never read past the map: rows beyond `map size / $BE` and columns beyond `$BE` belong to the next table
(blocks), i.e. garbage on screen. Clamp to `[0, $BE*32)` and draw a border outside.

## 3. Sprites: what is drawn and what is culled
- Bank 0 `sub_8688` (every frame, all 15 slots) computes screen X `$0485` = world X - camera X and screen Y
  `$0494` = depth - altitude - camera Y, and sets **bit 7 of `$04A3` when the entity is outside the 256-pixel
  window** (the high byte of world X - camera X is not 0). The metasprite drawer (bank 6 `sub_8c15`) skips those
  entities, and OAM X is only 8 bits (high).
- For widescreen the port needs its **own sprite pass**: re-run the frame-record decode (DATA_FORMATS section 7)
  for entities whose world X - camera X is in the wider window (e.g. -64..319), and draw them into a port-side
  sprite list (no 64-sprite / 8-per-line limit; the "no-flicker" option of M9 comes from the same pass). The
  game's own OAM stays untouched, so game logic (sprite 0 split, flicker rotation) is unchanged.
- Override points: after `sub_8688` (positions are final) and instead of presenting OAM. Do not change `$04A3`:
  game code reads the off-screen bit (e.g. AI that waits until it is on screen) (medium).

## 4. Spawns: will enemies pop in visibly?
- Spawn lists trigger on the **camera** (`$8E:$8D` >= trigger, DATA_FORMATS section 4), not on the screen edge.
  In the 2P TAS most spawns appear **inside** the normal window: median 85-200 px from the left edge per stage,
  90th percentile ~210-275 px; few start further right. So in a wider view most enemies appear where they
  always did (dropping in, walking out of doors); only those entering from the right (up to ~+280 px) will be
  seen to "pop" if the right margin is wider than ~24 px (medium: measured on all slot creations, including
  projectiles).
- Do **not** shift the spawn triggers to hide this: triggers decide timing and RNG use; changing them breaks
  lockstep with the original and TAS sync. Mask instead: fade the outer margins, or draw entities in the margins
  only once they have existed for a few frames.
- **Despawns**: live enemies are removed when they fall behind the left edge (187 of the removals in the 2P TAS
  happened off-screen left, often only 40-60 px behind). In a wide view they would vanish in plain sight in the left
  margin: same masking.

## 5. Gates (camera locks) and level ends
- `$A9` = `$1E` while an active enemy holds the camera (DATA_FORMATS section 3). The camera then stays put, but
  a wide view shows what lies ahead: areas whose enemies are not spawned yet (triggers are camera-based), and
  sometimes the next arena. Harmless visually; optional "lock margin": keep the extra right margin darker while
  `$A9` != 0.
- Camera X limits per level (bank 4 `sub_b3c9` tables `$B393`/`$B399`, level 5 `$8E` = `$11`): the wider view
  must clamp to the map, not to the game's camera range, or show a border.
- Section transitions (`$000D` = `$FE`) blank or fade the screen: hide the margins during them too.

## 6. Collision is unaffected
Collision uses world coordinates (DATA_FORMATS section 10), never the screen, except level 4, which samples
the background map at screen position + camera (`$BA99`): fine as long as the game still runs with its own
256-px camera.

## 7. Minimal implementation plan
1. Port-side background renderer: given camera X/Y, level, `$BE`, `$EA` and the PPU state, draw the 32x32 cells
   left and right of the 256-px picture from PRG (read from the player's ROM, using the meta ranges).
2. Port-side sprite pass from entity RAM + frame records for the wider window; optional no-flicker.
3. Margin masking rules: transitions, despawn/spawn edges, gate lock, map ends; per-stage on/off table (stage 3
   off, stage 4 starfield/off).
4. Test: picture inside the 256-px window must stay identical to the reference (shotdiff), RAM lockstep
   unchanged (the game code is not modified).
