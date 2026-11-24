# ROM and asset policy (v2)

Release model: we ship a tool that builds the game from the player's own ROM. Nothing from Nintendo/Rare/Tradewest.

## Released (allowed)
- Our original code: runtime, hardware layer, recompiler, renderer, upscalers, mod/override functions we wrote.
- `btdd.meta`: addresses, names, types, function boundaries, jump-table locations, data-format descriptions,
  in our own words. No ROM bytes, no instruction listings, no extracted strings/graphics/music.
- Third-party libraries under compatible licenses (SDL3 zlib, libtcc LGPL as a separate DLL).

## Never released, never committed to the port repo
- ROMs, bank dumps, `.cdl`/`.mss`/`.sav`/`.fm2`, music/WAV renders, screenshots of game graphics.
- `generated/` (recompiler output), `assets/` (anything extracted), the cache folder.
- The private decompile: `..\btdd-decomp` (ca65 source holding ROM bytes) and the readable C translations of
  routines (`private/` folder, gitignored here or kept in its own private repo).
- Redrawn/HD versions of the game's characters or art (players may load their own HD packs).

## Enforcement
- `.gitignore` + the release audit tool (scans release files for ROM byte runs >= 16 and ROM strings).
- The exe refuses any ROM whose SHA-1 is not `0218a66c6ee9887ec9115213a2f5b64055d79599`.

Not legal advice; get a lawyer's read before the first public release.
