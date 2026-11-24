# What the game needs from the hardware layer

Facts from btdd-decomp (docs/SOUND_FACTS.md, NES_REFERENCE.md, project_handoff_beginning.txt). Verify before relying.

## Cartridge
- iNES, mapper 7 (AxROM): 8 PRG banks x 32 KB, each mapped whole at `$8000-$FFFF`, each with its own vectors.
- Bank register write: bits 0-2 = PRG bank, bit 4 = single-screen nametable (A or B). No mirroring bit in the header.
- No CHR ROM: 8 KB CHR-RAM; tiles are uploaded by game code through `$2006/$2007`.
- Cross-bank calls: `ldx #lo / ldy #i / jsr $FFCB` runs `$80lo` in bank `byte[$FFB3+i]`; each bank starts with
  `jmp ($13)` + a `jmp $xxxx` table. The recompiler should turn this trampoline into a direct C call.

## CPU-timed effects
- Raw PCM: `$89AF` writes `$4011` ~11x per frame through the whole game. Audio needs per-write cycle timestamps.
- Vblank/sprite-0 polling loops on `PPUSTATUS` (bank 0 `$8180`, `$8190`, `$8587`, ...). Check which ones are
  sprite-0 hits (bit 6) used for status-bar scroll splits.
- NMI at `$FDE0` (bank 0); OAM DMA from page `$02` (`$FDF3`, `$FE12`, `$C5FD`).

## Sound driver
- Bank 3 `$8D00-$9400` (sub_89a9); music channel writer near `sub_8ec8`, SFX writer near `sub_8f55`.
- Music data per stage: docs/SOUND_FACTS.md in the decomp.

## Input
- `$4016` strobe at `$8DD7`/`$8DDB`. Two players. 2P mode A = friendly fire, mode B = no friendly fire.
