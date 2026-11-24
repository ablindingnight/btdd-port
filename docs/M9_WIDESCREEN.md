# M9: widescreen prototype (2026-11-24)

A 424x240 picture (16:9 with square pixels): the game's own 256x240 picture in the middle, 84 columns on each
side drawn by the port. Game logic is unchanged (the game still runs its 256-pixel camera); everything here is a
display mod. Background: `docs/WIDESCREEN_NOTES.md`, `docs/DATA_FORMATS.md` sections 1-4, 7.

## Mods (each toggleable: btdd.cfg `mod_* = on`, `--mod`, the settings menu; F10 = widescreen)
| mod | what |
|---|---|
| `widescreen` | margins from the level's block map (`src/platform/wide.c`) |
| `widesprites` | entities the game culls outside its window are drawn into the margins (its cull turned off for the picture only; the game still sets its off-screen flag, so AI is unchanged) |
| `widehud` | each player's half of the status bar moves to the edges of the wide picture |
| `widefade` | the outer 40 pixels of each margin are darkened (35-100%): spawns are camera-triggered and despawns happen behind the left edge, so enemies appearing or vanishing near the outer edges are less visible |

`btdd_play`: the SDL renderer path takes the wide picture and widens a windowed window (the M10 GPU shader path
stays 256 wide for now). `btdd_shot --mod widescreen ...` (or `--wide`) writes `wide_<frame>.png`.

## How the margins are drawn
- Background: the AxROM nametable is 256 pixels wide, so the margins come from the level data: block map,
  block set and attribute bytes located through bank 5's level tables (read from the player's ROM at run time)
  and the live RAM pointers (`$BC/$BD`, `$BA/$BB`, `$BE`, `$EA` sub-areas), drawn with the live CHR-RAM, palette
  and pattern-table select. World X = camera X (`$8E:$8D`) + screen x.
- Calibration instead of re-deriving every vertical offset rule: each frame the map is drawn under the visible
  picture and matched against the background the PPU drew (7 sample lines); the vertical offset dy (and a
  horizontal dx within +-8, seen -4..4) with the best match is used, last frame's first. Every line is then
  checked the same way: lines that are not the map (status bar, splits, text, transitions, generated or
  non-map backgrounds) keep black margins. Below 90 % match nothing is drawn; a missed search is retried every
  30 frames. Columns outside the map are black (map ends; never read past the map).
- The game hides its leftmost 8 pixels (PPUMASK); on widened lines they are filled from the map too (no seam).
- Sprites: for entities whose world X - camera X is outside 0..255, the port decodes the main entity drawer's
  frame records (bank 6 pointer tables by frame id `$03D1`, anchors, base/per-sprite attributes, sequential or
  explicit tiles, h/v flip, player marker palettes and player 2's tile block) and draws them into the margins
  with behind-background priority. Screen Y follows `sub_8688` (depth - altitude - camera Y) and is matched to
  the game's own `$0494` each frame (most common difference of the entities in the window). Not handled: the two
  "special" entity types, the second group of two player frames, 8x16 sprites, the bank 4 object drawer.

## Per stage (2P TAS sampled every 250 frames)
| stage (level) | result | why |
|---|---|---|
| 1 (3) | widens (section 04: 18/18 samples, 05: 5/8) | map 72 blocks; the rest are boss/transition frames |
| 2 (5) | **pillarboxed** (0/47) | the background does not follow the level map (camera X runs to ~10,000 px over a 2,048 px map; best map match 14-18 of 30 rows): it is built another way (repeating segments?), not found yet |
| 3 (2) | **pillarboxed** | vertical shaft, 256 px wide: there are no extra columns |
| 4 (4) | partly (section 06: 1/19, 07: 7/10) | section 06 is open space (black, nothing to add); 07's boss ship works; the RAM map is generated, so a wrong alignment is possible: calibration requires 90 % and dx within +-8 |
| 5 (8) | widens (42/42) | map 144 blocks; black at the map's left end |
| 6 (9) | widens (13/14) | map 32 blocks |
| 7 (6) | **pillarboxed** (0/5, best match 71 %) | map only 16 blocks (512 px) and does not match the picture well; little to gain |

Over the whole 2P TAS: 21,568 of 59,351 frames (36 %) widened (the rest: pillarboxed stages, cutscenes, menus,
transitions); 16,748 entity draws in the margins. 1P TAS (new-PPU model), first 20,000 frames: 19,083 widened.

What cannot widen cleanly: stage 3 (vertical), stage 2 (background not from the map), stage 7 (tiny map),
the open-space part of stage 4, map ends (black), status bar lines (black, or moved with `widehud`), screens
during transitions and text. Gates (`$A9` camera locks) show what lies ahead (enemies not spawned yet): harmless
in the samples; spawns triggered by the camera can pop in at the right edge (use `widefade`).

## Checks
- Mods off: every gate unchanged (`tools/check_all.py`, all compilers).
- `mods_test` (check_all `mods-test`): with the wide picture built every frame, RAM is identical to a console
  without it (2P 59,351 frames; 1P 20,000), and the wide picture's middle (columns 8-255) equals the game's own
  picture on every frame; margins are drawn on part of the frames.
- Real window: F10 toggles; the window widens and shrinks back.
- Screenshots (game pictures, outside the repo): `..\Gamefiles\Movies\m9\stage<N>_f<frame>_424x240.png`, all
  stages, with all four mods on.

## Next
Stage 2's background source; per-stage enable table (skip the search where it cannot work); the GPU path at
424 wide; the bank 4 object drawer and the special entity types in the margins; optional "lock margin" while
`$A9` holds the camera; a wider camera clamp at map ends (show a border instead of black).
