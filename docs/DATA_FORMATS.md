# Data formats (2026-11-24)

How Battletoads & Double Dragon (USA) stores its levels, spawns, graphics, palettes, text and sound. Addresses,
layouts and names only: no ROM bytes, values or text (docs/ROM_POLICY.md). Machine-readable ranges:
`meta/data_kinds.tsv` (written by `tools/data_kinds.py`, applied by `tools/export_meta.py` as `kind=` and
`fmt=` on `data` records; the `fmt` ids are the headings below).

**Confidence**: **high** = the code says so and an experiment or the read logs agree; **medium** = from reading
the code; **low** = a guess. Experiments use `btdd_headless --prg-poke B:ADDR=VAL` (in-memory PRG change) and
`--shot-every` pictures, kept under `..\Gamefiles\Movies\dataformats\`.

Conventions: `B:$ADDR` = bank B (each 32 KB AxROM bank is mapped whole at CPU `$8000-$FFFF`). Level = `$0010`
(the copy of `$000D` the logic uses), section = `$0015`. Stage order of the game: level 3, 5, 2, 4, 8, 9, 6
(stages 1-7); sections 04-05, 08-0A, 01-03, 06-07, 0B-0C, 0D, 0E (`docs/GAME_FACTS.md`).

## 1. Level layout: maps, blocks, attributes (bank 5)

The background is built from **blocks of 32x32 pixels = 4x4 tiles**. A level has one **block map**, one **block
set** and one **attribute byte per block**. Renderer: bank 5 `loc_a004` (column/row writer into the PPU buffer at
`$0120`, metatile cache `$019F-$01A8`), pointer setup `sub_a305` and `sub_a452`.

| fmt | where | layout | conf |
|---|---|---|---|
| `lvl_ptrs` | 5:`$A4A1-$A4EE` | six 13-entry tables indexed by level (0-12): block set lo `$A4A1` / hi `$A4AE`, attributes lo `$A4BB` / hi `$A4C8`, map lo `$A4D5` / hi `$A4E2`. `sub_a452` copies the first four to `$BC/$BD` (blocks) and `$BA/$BB` (attributes); `sub_a305` uses the map pointer. Index = level, or `$EA + 9` while `$EA` (a stage-5 sub-area flag) is set | high |
| `lvl_map` | bank 5, one range per level (see data_kinds.tsv) | **row-major grid of block numbers**, one byte per 32x32 block; row stride = section record byte 4 (`$BE`, blocks per row). Block address = map + row * `$BE` + column, with column = camera X / 32 and row = camera Y / 32 (`sub_a305` multiplies by `$BE`) | high (poke test: 3x8 map bytes of level 3 changed -> those 32x32 cells show the new block) |
| `lvl_blocks` | bank 5, per level | **16 bytes per block = 4x4 tile numbers, row-major** (bytes 0-3 = top row left to right). Address = `$BC/$BD` + block * 16 | high (poke test: bytes 0-3 blank the top tile row of every copy of the block, byte 5 = second row, second tile) |
| `lvl_block_attr` | bank 5, per level, right after the blocks | **one NES attribute byte per block** (a block covers exactly one attribute cell: four 16x16 quadrants x 2 palette bits); the renderer masks it with `$26` when the block grid is not aligned to the attribute grid | high for "one byte per block, read with the block", medium for "plain attribute byte" |

Sizes match the tables exactly: blocks end where the attribute bytes start, attribute bytes end where the next
level's map starts. Map sizes: level 3 is 72 blocks wide x 9 rows, level 5 64 wide, level 8 144 wide, level 9
32 wide, level 6 16 wide, level 2 is 8 wide x 232 rows (the vertical stage). Levels A-C (ending / special
screens) share one set; levels 0, 1 and 7 point at a dummy set (they are not playable: `M15_COVERAGE.md`).
**Level 4** (stage 4, the space stage) has its map in RAM at `$0600-$067F`: `sub_a452` clears it and copies
a 36-byte strip from 5:`$F1F5` to `$0624` (`lvl_map`, medium).

**Widescreen note (M9)**: the map is wider than the screen everywhere except level 2, so extra columns can be
drawn from the same map; the per-section camera limits (below) and the spawn triggers decide what a wider view
would reveal early.

## 2. Sections (bank 6)

Each level is split into sections (`$0015`, global numbering 1-14). Section start: bank 6 `sub_a0fb`.

| fmt | where | layout | conf |
|---|---|---|---|
| `sec_first` | 6:`$A0E2-$A0EB` | level -> its first section (used when `$0015` is 0 at level start) | high |
| `sec_recidx` | 6:`$A0ED-$A0FA` | section 1-14 -> byte offset of its record in `sec_record` | high |
| `sec_record` | 6:`$A19A-$A217` | **14 records x 9 bytes**: 0-1 initial camera X (hi, lo) -> `$8E:$8D`; 2-3 initial camera Y (hi, lo) -> `$90:$8F`; 4 map row stride in blocks -> `$BE`; 5 background palette number and 6 sprite palette number (indexes into `pal_ptrs`, loaded by `sub_a0c0`/`sub_a3c2`); 7 graphics set to load (bit 7 selects the bank 4 loader instead of bank 1, low 7 bits = set number); 8 a second graphics set (bank 1) | high for 0-4 (RAM after every section start of the 2P TAS matches), medium for 5-8 (code reading) |
| | 6:`$A0E0-$A0E1` | override indexed by `$93`: a non-zero entry replaces the section record (used by special screens) | low |

A **section change** inside a level is triggered by a **spawn-list record** (section 3): a record whose X high
byte has bit 7 set spawns a marker entity; its handler (bank 0 `sub_ee62`) clears the bit and copies the
record's X low byte into `$0015` (seen: frame 9338 of the 2P TAS sets section 09 this way). The loaders then
run for the new section (`sub_d073` resets the spawn pointer, `sub_a3f0` moves the players). Confidence: high.

## 3. Camera and scrolling (bank 4, bank 0)

| what | where | conf |
|---|---|---|
| Camera X = `$8E:$8D` (pixels, 16-bit), written to PPUSCROLL in the NMI (bank 0 `$81F9`/`$822A`/`$FE7A` area) | RAM | high |
| Camera Y / vertical position = `$90:$8F` (16-bit, signed; the vertical stage 2/level 2 grows it by `$900` per section) | RAM | medium |
| Follow logic: bank 4 `sub_b618`: the camera X moves 1-3 pixels per frame when the leading player's screen X passes `$80`/`$A0`/`$D0`, and backwards in the levels that allow it; per-level variants dispatched from bank 4 `sub_b78a` (level 4 and 9 have their own) | code | medium |
| **Camera lock ("gate")**: `$A9`. Active enemy routines (bank 0 `$FD91`, `$BCF9`, `$F5C4`, `$9CAB`) store `$1E` there every frame; the follow logic decrements it and does not scroll while it still reads `$1E`. When no enemy re-arms it, it counts down and the camera moves again. In the 2P TAS, 74 of the 88 camera stops of 30+ frames have `$A9` = `$1E` throughout (the others: nobody pushes the camera, or a map end). So a gate opens when the enemies that hold the camera are gone or have left their holding state, not by a counter | high |
| Hard limits: per-level end positions in bank 4 (`$B393`/`$B399` tables, `$8E` = `$11` stops level 5), `$EA` stage-5 sub-areas | code | low |

## 4. Enemy spawn lists (bank 3)

Spawner: bank 3 `sub_d0ec` (called every frame), list setup `sub_d073` (level/section start). Pointer `$AE/$AF`.

| fmt | where | layout | conf |
|---|---|---|---|
| `spawn_lvl_ptrs` | 3:`$D330-$D341` | per level: list lo `$D330,x`, hi `$D338,x` (the two tables overlap; levels 0-9) | high |
| `spawn_sec_tables` | 3:`$D0C1-$D0EB` | per section: `$D0C1,x` = number of records of the level list to run at once (`$80`+ = keep using the level list), `$D0CF,x` / `$D0DD,x` = lo / hi of the section's own list | high |
| `spawn_list` | bank 3 (data_kinds.tsv; 12 ranges, ~2.1 KB) | sequence of **11-byte records**: 0 entity type (-> `$03C2,x`; 1 and 2 are the players, placed in slot 0/1 if that player has lives); 1-2 trigger coordinate (hi, lo); 3-4 X (hi -> `$03EF,x`, lo -> `$03FE,x`); 5-6 Y (hi -> `$040D,x`, lo -> `$041C,x`); 7-8 Z (hi -> `$042B,x`, lo -> `$043A,x`); 9 -> `$0584,x` (behaviour parameter / timer); 10 low 2 bits -> `$04A3,x` (flags, e.g. facing), whole byte with the low nibble forced to `$F` -> `$051B,x` (HP). Special first bytes: `$FF` = jump (next 2 bytes = new list address, lo/hi), `$FE` = hold (the list stops here until a section change resets the pointer) | high |

**Trigger rule.** Records are processed strictly in order, at most one per frame, and only when a free entity
slot exists (bank 0 `$FF20`; level 4 also keeps slots `$0E`+ free). A record waits until the camera reaches its
trigger: camera X (`$8E:$8D`) >= trigger. In levels 2 and 5 the low 2 bits of the trigger's low byte pick the
axis/direction: 0 = camera X >= trigger, 1 = camera X <= trigger, 2 = camera Y >= trigger, 3 = camera Y <=
trigger. Experiment: raising the trigger of the 3rd record of stage 1 out of reach stops every later spawn of the
stage (no entity written by `$D17B` for 3,000 frames). Confidence: high.

**Player start positions** (bank 5 `sub_a3f0`, at a section start):

| fmt | where | layout | conf |
|---|---|---|---|
| `start_idx` | 5:`$A38C-$A39A` | section -> byte offset into `start_pos` (0 = players keep their position) | high |
| `start_pos` | 5:`$A39B-$A3EF` | per section 2 x 6 bytes (player 1, player 2): X hi/lo, Y hi/lo, Z hi/lo (hi bytes `$03EF`/`$040D`/`$042B`, lo bytes `ent_x`/`ent_y`/`ent_z`) | medium |

## 5. Palettes (bank 6)

| fmt | where | layout | conf |
|---|---|---|---|
| `pal_ptrs` | 6:`$A3FA-$A415` | 14 word pointers, indexed by section record bytes 5 (background) and 6 (sprites) | high |
| `pal_16` | 6:`$A416-$A4F5` | 14 palettes x 16 NES colour indices, written to PPU `$3F00`/`$3F10` and to the RAM copy `$5B-$7A` (fades work on the copy) | high |

## 6. Tile graphics (CHR-RAM uploads)

The cartridge has CHR-RAM: every tile is copied from PRG to the PPU.

| fmt | where | layout | conf |
|---|---|---|---|
| `chr_set` | 1:`$8193` table (+ sets after it), 7:`$825B` table | word pointers by set number; a set = PPU target address (2 bytes) then entries of 4 bytes (tile count, method, source pointer); method 0 ends the set; methods select a handler from a 4-entry table (1:`$8094`, 7:`$815C`) | medium |
| `chr_rle` | banks 1, 4, 7 (data_kinds.tsv) | **escape-coded stream**: 2 header bytes give two escape values; in the data an escape byte is followed by a run length and writes that many `$00` (first escape) or `$FF` (second escape) bytes; every other byte is copied. Ends after tile count x 16 output bytes. Decoders: 1:`sub_809c`, 7:`sub_8164`, 4:`sub_802b` | high (code + every byte read by these loops) |
| `chr_raw` | bank 1 (`sub_8174`), bank 2 (most of the bank) | plain 16-byte tiles. Bank 2 is streamed during play by an unrolled copy routine in RAM (`$0782-$07FF`, copied there from 7:`$805F` by 7:`sub_801b`): 16 bytes per call, i.e. one tile, used for animation frames | high |

## 7-9. Metasprites, text/font, music

Sections 7-9 are filled from the read-hook analysis in progress (`..\Gamefiles\Movies\dataformats\`).

## Not found yet
- **Collision / floor height**: no per-block collision table was found; the attribute byte per block is the only
  other per-block data. Entity bounds look like level-specific code (bank 0/4). Open.
- Spawn lists of the unused levels 0, 1, 7 point at code or garbage (as their maps do).
