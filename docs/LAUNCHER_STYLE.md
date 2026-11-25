# Launcher style sheet: Pond to Pavement (M11)

Decided with the user in the design session of 2026-11-24. The launcher code and the in-game menu follow this file.
Mockups: `docs/design/launcher-screens.html` (all screens; open `#only-<screen>` for one 1280x720 screen, which is
how `docs/images/launcher-*.png` were made until the launcher's own `--ui-shot` replaces them) and
`docs/design/launcher-directions.html` (the three directions we chose from; A won).

## Identity
- **Name**: Pond to Pavement (renamed from Knuckle Drive on 2026-11-24 by the user; trademark search before release). **Tagline**: "Your ROM. Our engine. Two
  fists." Short form: "Your ROM. Our engine."
- **Direction**: "Splash Page": the cover of a 1993 comic. Ink outlines, halftone dots, tilted panels, a burst-shaped
  PLAY button, on a toad-green field with orange-red and yellow accents and a deep-space emblem.
- **Home headline**: "Two Brothers, Three Toads... Zero Mercy!" (user's line). Three lines: "Two Brothers," /
  "Three Toads..." / "Zero Mercy!" (last line yellow).
- **Logo**: a fist bump: a green fist from the left (cuffed in red) meets a human fist in a paper-white wrist wrap
  from the right, with a yellow impact burst, over a disc that is half night city (skyline with lit windows, a moon)
  and half pond (water, ripples, a lily pad): pond to pavement. Hands only, no faces or characters. Drawn in
  `src/launcher/theme.c` (`theme_emblem`). Wordmark: "POND TO" over "PAVEMENT" in Bangers, yellow with a 4 px ink
  outline, an orange-red offset and an ink offset, tilted -4°. (The first design's rocket fist is in the mockups.)
- **Never** in the launcher, package, icon or page: the game's characters, logos, sprites, screenshots, music, or the
  Battletoads / Double Dragon / Rare / Tradewest / Nintendo names as branding. The game title appears only as plain
  text where we say which ROM is needed (Your game row, first-run step 1, About). "Toads" and "brothers" in the
  headline are ordinary words, never styled like the game's logo.

## Colour tokens
| token | hex | role |
|---|---|---|
| `green` | `#2f9a3a` | field (radial: `#3fb64a` centre-left -> `green` 45% -> `green-deep` edges) |
| `green-deep` | `#17611f` | nav tabs, vignette, depth |
| `halftone` | `#0b3d12` | halftone dots on the field |
| `ink` | `#0b0b14` | outlines, shadows, title bar, code box, in-game menu body |
| `paper` | `#fff8e7` | cards and panels, text on green (always with ink outline when large) |
| `yellow` | `#ffd21f` | primary: PLAY, selected item, buttons, caption stickers, logo |
| `red` | `#ff4a1c` | accent: section labels, page title plate, focus arrow, NEXT START sticker, Leave |
| `cyan` | `#5fd8ff` | status: Verified / OK badges, used save slots, in-game values, toasts |
| `space` | `#24123f` | emblem disc only |
| `grey` | `#c9c3b4` | disabled / optional badges |
| `ok` | `#22c46b` | status dot good (with a word next to it, never colour alone) |
| `warn` | `#ff8a1c` | status dot attention |
Rules: text on `paper` is `ink` (secondary `ink` at 63% alpha); text on `green` is `paper` with a 3 px ink stroke and an
ink drop shadow, or sits on a sticker; `yellow` and `cyan` carry `ink` text only. Every coloured state also has a word
or shape.

## Type (all SIL OFL 1.1, shipped as files next to the launcher)
| role | font | sizes (at 1280x720) |
|---|---|---|
| display | Bangers | 96 / 66 (Home headline) / 44 (page plate) / 22 (buttons, links) |
| labels | Barlow Condensed 700-800, caps, +0.08-0.14 em | 21 (row value) / 17 (nav) / 15 (row label) / 13 (badge) |
| body | Barlow 500 | 15 / 13 (hints under a setting) |
| mono | JetBrains Mono 400-600 | 30 (join code) / 13 (paths, versions, log) |
| in-game | 8x8 pixel font (Press Start 2P baked to an atlas, or our own 8x8) | 1 NES pixel = 1 font pixel |
Scale steps used: 13, 15, 17, 21, 22, 30, 44, 66, 96. Line height 0.95 for display, 1.1-1.2 for labels, 1.45 body.

## Spacing, lines, angles
- Base unit 4 px; steps 4, 7, 10, 14, 22, 30. Screen margin 34-70 px, title bar 46 px, settings panel 946x590 at
  (300, 70).
- Borders: 3 px ink everywhere (2 px on badges and key chips, 4 px dashed paper on the drop zone).
- Shadows are flat offsets, never blur: 3 px (tabs), 4 px (buttons, stickers), 5-7 px (cards, selected nav).
- Tilts give the comic feel: stickers and headlines -2°, logo -4°, Home rows straight (small tilts on long boxes look jagged), selected nav
  shifted 10 px right (not tilted: straight edges read cleaner). Settings rows themselves stay straight so they read fast.
- Dashed 2 px ink-at-19% separators between setting rows.

## Backdrop
1. Green radial field. 2. Halftone: 14 px grid of 2.4 px dark-green dots, masked to a soft circle (Home: around the
emblem; settings: bottom-right corner). 3. Rays: conic stripes (6° white at 6% / 8° clear), centred behind the emblem
(Home) or the bottom-right corner (settings), turning once per 80 s. No game art, no photos.

## Components
| component | look | used on |
|---|---|---|
| Title bar | ink, wordmark in Bangers 22 yellow, version, breadcrumb "/ SETTINGS" with red slash, window buttons | all |
| Caption sticker | yellow, 3 px ink, 4 px shadow, caps +0.14 em, tilted -2° | status line, step counter |
| Card row | paper, 3 px ink, 7 px shadow; label (red caps) / value + small line / action link (Bangers, red underline) | Home |
| PLAY burst | 22-point star polygon, ink outline 7 px, yellow fill, "PLAY!" red with ink stroke; idle bounce 1.6 s; disabled = greyscale 55% | Home, First run |
| Nav tab | green-deep, paper caps + 20 px line icon; selected = yellow, tilted, larger shadow | settings sidebar |
| Page plate | red block, Bangers 44 yellow with ink stroke, tilted -2°, overlapping the panel's top-left corner | settings |
| Setting row | label 20 / hint 13 left, control right (330 px); focused row = yellow 33% fill, 3 px ink outline, red arrow outside | settings |
| Choice | white box, `‹ value ›` with red chevrons, min 200 px | settings |
| Toggle | two-cell ON/OFF; ON = yellow cell, OFF = ink cell | settings |
| Slider | white track with halftone-yellow fill and ink end line, value on the right | Audio |
| NEXT START sticker | red, paper caps 12, tilted -3° | changes that apply on the next start |
| Button | Bangers 22, yellow (primary) / paper (secondary) / cyan (copy, load) / red (leave, destructive) | all |
| Badge | 2 px ink, caps 13: cyan OK, red warning, yellow hotkey, grey optional / coming | all |
| Stamp | Bangers 18 in an outline box, tilted -6°: green GOT IT / VERIFIED, red WORKING, grey NEXT / SKIP | First run, Builds |
| Progress | white track, red/yellow candy stripes moving 1 s per stripe | First run |
| Preset card | 6 across, swatch of an abstract test pattern rendered with that preset, selected = yellow, lifted 3 px | Graphics |
| Key chip | white, 2 px ink, 5 px bottom edge; capturing = blinking yellow "PRESS A KEY" | Controls |
| Save slot | tile 0-9, used = cyan stripes + stage label, current = red outline | Gameplay |
| Mod card | paper, toggle top-right, short text, badges (PICTURE / GAMEPLAY / hotkey); on = red offset shadow | Mods |
| Join code box | ink, JetBrains Mono 30 yellow, +0.08 em | Online |
| Status line | dot (green / yellow / red, ink ring) + text + badge | Online |
| Ping bars | 15 cyan bars, newest right | Online |
| Log view | ink box, mono 12.5, time yellow, paths cyan, user folder shown as `%USERPROFILE%` | Logs |
| Hint bar | bottom right: key chips + words (A change, B back, LB/RB page) | all |

## Icons (24 px grid, 2.4 px round strokes, drawn by us)
home, display, graphics (star), audio, controls (pad), gameplay (rewind arrow), mods (puzzle piece), online (globe),
builds (wrench), logs (sheet), about (i). Still to draw: verified check, warning, folder, copy, rebuild (circular
arrows), play triangle, gamepad glyphs A / B / X / Y / LB / RB / Start / Back, keyboard key, ROM cartridge (generic
rectangle with contacts, no console shape), sprite pack (generic brush), app icon (the rocket fist on the space disc,
16/32/48/256).

## Motion
- Durations: 120 ms (focus move), 200 ms (panel slide), 320 ms (page turn); ease-out `cubic-bezier(.2,.8,.2,1.2)`
  for the slight "slam" overshoot.
- Screen change: the new panel slams in from 24 px right with a 2° overshoot; the page plate drops in 80 ms later.
- Idle: PLAY bounce (scale 1.04, -1.5°, 1.6 s), rays turn (80 s), working stamp pulses. Nothing else moves at rest.
- Reduced motion (system setting or launcher toggle): no idle motion, cross-fades only.

## In-game menu (btdd_play overlay, `src/platform/menu.c`)
Drawn at NES resolution (256x240) in RGB, scaled with the picture, so it matches at every window size:
- body `ink`, 2 px `yellow` border, 4 px `red` offset shadow (bottom-right), title tab `yellow` with `ink` text
  sitting on the top border;
- items in the 8x8 font, `paper`; values and `›` in `cyan`; selected line = `yellow` bar with `ink` text, `red`
  value and a `red` triangle at the left;
- a dotted `paper`-at-25% line above the help text (`grey`); toasts ("SAVED TO SLOT 3") in a `cyan` box, top-right;
- same page order and names as the launcher's settings; 31 columns (MENU_COLS).

## Voice
Short, loud, friendly; say what happens ("Rebuild", "Copied", "Nothing is sent anywhere"). Comic exclamation only in
headlines and PLAY. Explain fixes plainly in errors ("This ROM is the Europe version; the USA one is needed").

## Screens (`--ui-shot <name>` once the launcher exists)
home, first-run, display, graphics, audio, controls, gameplay, mods, online, builds, logs, about, plus `in-game`
from `btdd_shot` with the menu open.
