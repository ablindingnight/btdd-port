# Pond to Pavement

**Your ROM. Our engine. Two fists.** A native PC port of a 1993 NES co-op brawler, built on your computer from your
own cartridge dump.

![Pond to Pavement launcher: ready to play](docs/images/launcher-home.png)

## How to play
1. You need your own backup of the **Battletoads & Double Dragon (USA)** NES cartridge as a `.nes` file (or a `.zip`
   that holds it). Pond to Pavement does not include the game and can't get it for you.
2. Download the latest release and unzip it anywhere. Start `PondToPavement.exe`.
3. Drag your ROM onto the launcher, or pick it.
4. The launcher checks it's the USA cartridge, compiles the game into a native program, and runs a quick self-check
   (under a minute). Then press **PLAY**.

![First run: building your game](docs/images/launcher-first-run.png)

Everything is in the launcher and in game (Esc): window size, widescreen, CRT and smoothing presets, no sprite flicker,
two players on keyboard and gamepads, save states, rewind, fast forward, the game's own cheat codes, and online 2-player
with join codes (no port forwarding in most homes).

| ![Graphics settings](docs/images/launcher-graphics.png) | ![Online play](docs/images/launcher-online.png) |
|---|---|
| ![Controls](docs/images/launcher-controls.png) | ![Gameplay: save states, rewind, cheats](docs/images/launcher-gameplay.png) |

**If something goes wrong**: the launcher's **Logs** page lists every run, and *Save report* makes a folder with the
logs and your settings (your user name removed). Nothing is sent anywhere; attach the folder to an issue.

## What's in the download
The launcher, the game engine (`btdd_play.exe`), the compiler it uses on first run (`libtcc.dll`), the game description
file (`btdd.meta`: names, addresses and code layout, no bytes from the cartridge), and our fonts and art.

## No game data here
This project contains no code, graphics, music or text from the cartridge, and no ROM. On first run the engine reads
your ROM and compiles it on your PC; graphics and sound come from your ROM while you play. Every release is scanned
for cartridge bytes before upload. Not affiliated with or endorsed by the game's publishers or by Nintendo; game
names are used only to say which ROM you need.

## Licence
MIT. The launcher art (rocket fist, logo, backdrop) is our own and MIT too. Fonts: SIL Open Font License 1.1.

## For developers
Build, layout and tests: see the main README sections below this draft (`README.md`), `docs/ARCHITECTURE.md`,
`docs/ROADMAP.md`. Launcher design: `docs/LAUNCHER.md`, `docs/LAUNCHER_STYLE.md`.
