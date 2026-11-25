# Launcher (plan, 2026-11-24)

Goal: players get one polished app that takes their own ROM, gets the game ready, holds every setting, and starts
it with PLAY. Model: the user's Nice Crash launcher (`C:\Users\theop\claude\Excite Truck\port\host\src\launcher`,
same author, CC0; its architecture and screen flow can be reused, its art and name cannot).

## Screens (mirroring Nice Crash, adapted to an NES ROM)
- **Home**: "Your game" (ROM file, SHA-1 Verified badge), "Your build" (port version, btdd.meta version, native
  cache state, Rebuild), "Newest version" (update check off until there is an update host), big **PLAY** button,
  footer links Settings / Controls / Logs / About, keyboard and gamepad navigation (A/Enter = play).
- **First run** (a checklist on a worker thread, progress per step):
  1. pick or drag the ROM (`.nes`, or a `.zip` that contains it); 2. verify the USA SHA-1 (Europe later);
  3. recompile with the shipped `btdd.meta` + embedded libtcc into the native cache (M5); 4. self-check: boot to the
  title in the headless core and compare a few frames native vs interpreter; 5. optional "Enhance my game"
  (upscaled tiles / HD pack import, M10); 6. "Ready to play".
- **Settings pages**: Game files, Display (window 1x-6x / fullscreen / borderless, aspect pixel-perfect 8:7 / 4:3 /
  stretch, integer scaling, vsync), Graphics (filters, CRT/NTSC shaders, pixel-art upscalers, palettes, sprite
  flicker fix, widescreen; "Next start" tags like Nice Crash), Audio (volume, per-channel mute, latency), Controls
  (2 players, keyboard + gamepads, rebinding, turbo), Gameplay (mods/overrides toggles, M7; save states, rewind),
  Builds (native vs interpreter, renderer, rebuild cache), Logs (runs list, Save report with the user name removed,
  nothing sent anywhere), About (licences, "your ROM, our engine", no-affiliation notice).
- Settings live in `%LOCALAPPDATA%\btdd\settings.ini` (only changed values), shared with `btdd_play`; in-game
  overlay (Tab/Esc) for the common ones.

## Rules
- Our own name, logo and art: no Battletoads / Double Dragon / Rare / Tradewest / Nintendo branding or character
  art in the launcher or the package (trademarks). The game's title only appears as plain text where we say which
  ROM is needed. (Decide a project name before release.)
- Package = launcher + game exe + libtcc.dll + btdd.meta + fonts/art we own. `tools/release_audit.py` scans the
  whole package before any upload (as Nice Crash's `scan_package.py` does).
- Testable headless: `--ui-shot <screen> out.png [w h]` renders any screen offscreen, `--launch-game` starts the
  game as PLAY does (both exist in Nice Crash and made its UI work reviewable by agents).

## Design session (user wish, 2026-11-24)
Once the port is at a good place for it (game playable natively from the player's ROM, settings stable), start a
dedicated design session for the launcher before building its screens:
- Art direction that matches the game's feel (1993 beat-'em-up: space ship hull, gritty sci-fi, bold comic-book
  energy, CRT-era colours), done the way Nice Crash did its own star/tilt-ring/truck art: original shapes,
  typography and backdrop, inspired by the mood, never the game's characters, logos or sprites.
- Deliverables: project name + logo, colour palette and fonts (open licences), home/first-run/settings mockups,
  backdrop art, PLAY button and icon set, and a style sheet the launcher code follows.
- A project page like https://github.com/ablindingnight/nice-crash: README with launcher screenshots (made with
  `--ui-shot`, so they never contain game graphics), How to play, What's in the download, No-Nintendo-data
  statement, licence, and For developers.

## Tech choice (decide when the milestone starts)
- Nice Crash uses GDI+ (Windows only). Options: reuse its launcher code (fastest, Windows-only), or rebuild the same
  screens on SDL3 + our renderer (cross-platform, shares input/gamepad code with btdd_play). Default: SDL3, reusing
  Nice Crash's screen flow, task checklist and settings/logs code where it ports cleanly.

## Design session result (2026-11-24)
Name **Knuckle Drive** ("Your ROM. Our engine. Two fists."), direction "Splash Page" (comic cover) recoloured toad
green on the user's request, Home headline "Two Brothers, Three Toads... Zero Mercy!". Style sheet:
`docs/LAUNCHER_STYLE.md`. Mockups: `docs/design/launcher-screens.html` (13 screens incl. the in-game menu),
`docs/design/launcher-directions.html` (the three directions), `docs/design/project-page.html` +
`docs/design/README_DRAFT.md` (release README). Placeholder screenshots `docs/images/launcher-*.png` come from the
mockup page (`#only-<screen>`) until `--ui-shot` exists. Open items: trademark search for the name; the remaining
icons (list in the style sheet); an 8x8 font for the in-game menu; app icon sizes.
