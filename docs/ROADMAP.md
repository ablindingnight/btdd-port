# Roadmap (v2)

| # | Milestone | Done when |
|---|---|---|
| M0 | Skeleton | `btdd.exe ROM` verifies SHA-1 and prints the banks' vectors (done 2026-11-23) |
| M1 | Reference interpreter | exact 6502 + AxROM bus, headless; fm2 playback; RAM matches FCEUX over the 2P TAS (done 2026-11-23: identical on all 59,351 frames; 1P TAS too with the new-PPU model, docs/M1_INTERPRETER.md) |
| M1.5 | Coverage engine | headless coverage recorder, snapshots, stage warp/god mode, bots, coverage-guided exploration (docs/COVERAGE_PLAN.md); feeds new logs to btdd-decomp |
| M2 | Window + picture + input | SDL3 window, scanline PPU, keyboard/gamepads; playable on the interpreter |
| M3 | Sound | APU incl. cycle-stamped `$4011` PCM; matches the FCEUX APU log |
| M4 | Metadata format | `btdd.meta` (text, diff-friendly): functions, jump tables, trampolines, data regions, names, types; exported automatically from btdd-decomp; contains no ROM bytes |
| M5 | Load-time recompiler | ROM + meta -> C -> libtcc -> native, cached; interpreter fallback; lockstep clean over the whole 2P TAS |
| M6 | Qwen pipelines | names/types (decomp loop v2), private routine-to-C with differential tests, data-format notes; unattended for hours |
| M7 | Override/mod system | hook by ROM address (replace/wrap), per-mod tests, mod toggles in config |
| M8 | PC features | config file, rebinding, 2P on gamepads, fullscreen/scaling, save states + rewind, fast boot |
| M9 | Quality extras | no sprite flicker, widescreen (extra columns from level data), optional difficulty/lives tweaks |
| M10 | Graphics upgrade (incl. custom sprite packs: player-made replacement sprites swapped in by tile hash or frame id, Mesen-HD-pack compatible, toggled in the launcher; start with research: match the best options of recent official retro collections and native PC ports; 2x/3x/4x sizes with real enhancement: pixel-art upscalers, CRT/NTSC shaders via the SDL3 GPU API, borders) | renderer from game state, runtime upscalers on the player's tiles, HD tile-pack loader |
| M11 | Launcher | starts with a design session (art style matching the game's mood, own name/logo/art, page with screenshots like github.com/ablindingnight/nice-crash); then a polished app modelled on the Nice Crash launcher (docs/LAUNCHER.md): home with ROM/build status and PLAY, first-run checklist (verify ROM, recompile, self-check, optional enhance), settings pages for display/graphics/audio/controls/mods, logs + save report, own name and art |
| M13 | Online 2-player (netplay) | play 2P over the internet: rollback netcode (GGPO-style) on our deterministic core: exchange only inputs, save/restore states to re-simulate late inputs; both sides verify the same ROM SHA-1 + exe/meta/mod settings; per-frame RAM checksum for desync detection; direct IP first, relay/NAT traversal later; lobby in the launcher |
| M12 | Release | launcher + game exe + `btdd.meta` + libtcc.dll; release audit clean on the whole package (no ROM bytes/strings); player supplies ROM |
| M14 | Genesis / Mega Drive version (second version, after this project wraps) | research spike first (TAS movies, docs, ROM layout); then a 68000 + Z80 core with VDP/YM2612/PSG matched exactly, a 68k recompiler and M6-style C pipeline; reuse btdd.meta, release audit, packaging, rollback netplay, save states/rewind, mods, shaders; selectable in the same Pond to Pavement launcher ("NES" / "Genesis" game switch, each with its own ROM check and cache) |

Order: M1-M3 make it playable; M4-M5 make it native from the ROM; M6 runs in the background the whole time.
Parallel track: btdd-decomp keeps naming and logging (levels 4+, 1P, mode B, game over, ending).
