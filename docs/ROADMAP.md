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
| M10 | Graphics upgrade (start with research: match the best options of recent official retro collections and native PC ports; 2x/3x/4x sizes with real enhancement: pixel-art upscalers, CRT/NTSC shaders via the SDL3 GPU API, borders) | renderer from game state, runtime upscalers on the player's tiles, HD tile-pack loader |
| M11 | Launcher | polished app modelled on the Nice Crash launcher (docs/LAUNCHER.md): home with ROM/build status and PLAY, first-run checklist (verify ROM, recompile, self-check, optional enhance), settings pages for display/graphics/audio/controls/mods, logs + save report, own name and art |
| M12 | Release | launcher + game exe + `btdd.meta` + libtcc.dll; release audit clean on the whole package (no ROM bytes/strings); player supplies ROM |

Order: M1-M3 make it playable; M4-M5 make it native from the ROM; M6 runs in the background the whole time.
Parallel track: btdd-decomp keeps naming and logging (levels 4+, 1P, mode B, game over, ending).
