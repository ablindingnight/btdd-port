# Architecture (v2, 2026-11-23)

## Two products, one codebase
| | **Public: `btdd.exe`** (released) | **Private: the decompile** (never released) |
|---|---|---|
| What | One exe. The player points it at their own ROM; it builds the native game from it on first run | Readable, named C of every routine + the ca65 rebuild in `..\btdd-decomp` |
| Contains | Only our original code + **metadata** (addresses, names, types, function boundaries, jump tables, data formats). Zero ROM bytes, zero translated game code | Translations of the original code (derivative: stays local) |
| Purpose | Play the game natively with PC extras | Understand the game so the metadata, mods and extras are correct |

This is the Zelda64Recompiled model: the release ships a recompiler + runtime + mods, the player's ROM supplies
the game. Every game routine in the running exe is generated on the player's machine from their ROM.

## Public exe: what happens on first run
1. Player picks their ROM. SHA-1 must be `0218a66c...9599` (USA). Nothing else is accepted.
2. **Load-time static recompiler** (our code) reads the ROM using `btdd.meta` (function boundaries, jump tables,
   bank trampolines, data regions) and emits C for every routine, compiled in-process by embedded **libtcc**
   (TinyCC as a DLL, LGPL) into native code. Result is cached in the player's folder (`%APPDATA%\btdd\cache`),
   tied to the ROM hash and exe version. Fallback if anything is unknown: the built-in 6502 interpreter.
3. **Runtime** runs the native routines on our hardware layer (PPU/APU/AxROM/input) at 60 Hz.
4. **Mods/overrides**: our own C functions hooked at ROM addresses (`btdd.meta` lists them), replacing or wrapping
   a routine: widescreen camera, flicker fix, extra save slots, debug menu, etc. Written by us, not translated.
5. **Renderer**: draws the NES picture from the hardware layer, then extras: integer/CRT scaling, algorithmic
   upscalers run on the player's own tiles at runtime (xBRZ/ScaleFX-style), and a loader for HD tile packs that
   players supply themselves (Mesen HD-pack compatible). We do not ship redrawn characters or art.

## Private decompile: why it still matters
- Every name/type/boundary in `btdd.meta` is proven by it. Better understanding = better mods and extras.
- Its readable C is the spec for writing original overrides (we write new code with the old behaviour in mind,
  verified by tests, instead of shipping the translation).
- It is where the local model does most of its work (below).

## Verification (all automatic)
- **Reference interpreter** = ground truth. Lockstep: run reference vs recompiled build on the TAS movies, compare
  RAM every frame. The recompiler is correct when the whole 2P TAS matches.
- **Per-routine differential tests**: capture thousands of entry states per routine from TAS playback, run asm vs
  C (private decompile) or asm vs override (mods that must keep behaviour), compare RAM/registers/hardware writes.
- **Release audit**: a tool scans every release file for any byte run (>= 16 bytes) that also appears in the ROM,
  and for strings from the ROM. A hit blocks the release.

## Local model (Qwen3-Coder-30B) does the bulk, Python does the rest
Qwen never edits files. A Python driver builds a small prompt, Qwen answers in a fixed format, Python validates,
tests, applies and commits, and retries with the exact error. Claude only sees what fails twice.
Qwen jobs, in order of value:
1. **Names/types for metadata** (already running in btdd-decomp): routines, RAM, tables, struct views.
2. **Private decompile**: one routine -> readable C, auto-checked by the differential test.
3. **Data format notes**: level/enemy/metasprite table layouts from facts + tests (needed for widescreen/HD).
4. **Override drafts** for extras, checked by tests that define the intended behaviour change.

## Timing-sensitive spots (see HARDWARE_NOTES.md)
- `$89AF` raw PCM via `$4011` ~11x/frame: generated code counts cycles so the APU can timestamp writes.
- `PPUSTATUS` wait loops / possible sprite-0 splits: the PPU renders per scanline from timed register writes.
