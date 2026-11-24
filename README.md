# btdd-port: native PC port of Battletoads & Double Dragon (NES)

A native Windows (later Linux/macOS) build of *Battletoads & Double Dragon: The Ultimate Team* that runs from
**your own backed-up ROM**. This repository contains no Nintendo/Rare/Tradewest code or data: on first run the
exe recompiles the game from the player's ROM (guided by `btdd.meta`, which holds no ROM bytes), and graphics/
music are read from the ROM at run time. The readable C decompile is private and never released
(docs/ARCHITECTURE.md, docs/ROM_POLICY.md).

Sibling projects in `C:\Users\theop\claude\Battle\`:
- `btdd-decomp\` byte-identical ca65 rebuild (labels, code/data split, RAM names): the port's source of truth.
- `Gamefiles\` ROMs, Mesen 2, save states, code/data logs, TAS movies (never in any git repo).
- `tools\` cc65, FCEUX, Ghidra (+ GhidraNes, GhidraMCP), test ROMs, FCEUX source (reference only).

Start with `docs/ARCHITECTURE.md`, then `docs/ROADMAP.md`. Agents: read `AGENTS.md`.

## Toolchains (installed 2026-11-23)
The C code must build without warnings with all three compilers (C11; `/W4` or `-Wall -Wextra`).

| Tool | Version | Install |
|---|---|---|
| MSVC (cl) | 19.51.36263 | Visual Studio 18 Community, C++ workload (its bundled CMake 4.3.1 drives the VS generator) |
| LLVM / clang | 23.1.6 | `winget install --id LLVM.LLVM -e` (to `C:\Program Files\LLVM\bin`) |
| gcc | 16.2.0 (MSYS2 UCRT64, Rev7) | `winget install --id MSYS2.MSYS2 -e`, then `C:\msys64\usr\bin\bash.exe -lc "pacman -Syu --noconfirm"` (run it twice: the first run updates pacman itself) and `C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm --needed mingw-w64-ucrt-x86_64-gcc"` (to `C:\msys64\ucrt64\bin`) |
| CMake | 4.4.4 | `winget install --id Kitware.CMake -e` |
| Ninja | 1.13.2 | `winget install --id Ninja-build.Ninja -e` |
| SDL3 | release-3.4.20 | fetched by CMake (FetchContent, pinned tag); linked by `btdd_play` and `btdd_sdl_probe`. `-DBTDD_SDL=OFF` skips the download (and those targets) |

## Build
MSVC (Visual Studio generator):
```powershell
$vscmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $vscmake -S . -B build
& $vscmake --build build --config Release
```
clang and gcc (Ninja; a new shell has them on PATH after the installs):
```powershell
cmake -S . -B build-clang -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build-clang
cmake -S . -B build-gcc -G Ninja -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_BUILD_TYPE=Release
cmake --build build-gcc
```
Targets: `btdd_play` (the game in a window), `btdd` (M0: ROM check), `btdd_headless` (reference
interpreter), `btdd_apu_check` / `btdd_shot` (sound and picture checks against FCEUX), tests
(`nestest_check`, `cpu_call_test`, `apu_synth_test`), `btdd_sdl_probe` (not in the default build).

## Play (M2)
```powershell
.\build\Release\btdd_play.exe "..\Gamefiles\Battletoads-Double Dragon (USA).nes"
```
Player 1: arrows, X = A, Z = B, Right Shift = Select, Enter = Start. Player 2: W/A/S/D, H = A, G = B, T = Select,
Y = Start. Gamepads work too. Esc quits, P pauses, F11 fullscreen, F2 square pixels / 4:3, F12 screenshot.
Keys, scale and the ROM path are set in `%APPDATA%\btdd\btdd.cfg` (created on first run). Details:
`docs/M2_PLAY.md`.

## Reference interpreter (M1)
```powershell
.\build\Release\btdd_headless.exe "..\Gamefiles\Battletoads-Double Dragon (USA).nes" --movie ..\Gamefiles\Movies\tasvideos-4323M-2p.fm2 --dump-ram ..\Gamefiles\Movies\m1\interp_2p.bin --dump-every 1
python tools\ramdiff.py ..\Gamefiles\Movies\m1\fceux_2p.bin ..\Gamefiles\Movies\m1\interp_2p.bin --names ..\btdd-decomp\src\ram.inc
.\build\Release\nestest_check.exe ..\tools\testroms\nestest.nes ..\tools\testroms\nestest.log
```
For the 1P TAS add `--ppu new` (FCEUX new-PPU timing). How it is timed, how the FCEUX reference dumps are made, and what it does not model yet: `docs/M1_INTERPRETER.md`.

## Folder map
```
btdd-port\
  AGENTS.md            rules for every agent (Claude, local model)
  CMakeLists.txt       build; SDL3 is fetched by CMake (only the probe target links it until M2)
  docs\                ARCHITECTURE, ROADMAP, ROM_POLICY, HARDWARE_NOTES, M1_INTERPRETER
  recomp\              Python: btdd-decomp src\*.s + logs -> generated\*.c (6502 -> C)
  generated\           recompiler output (gitignored: derived from the ROM)
  src\rom\             ROM loading, SHA-1 check, bank access
  src\runtime\         6502 interpreter (fallback + reference), fm2 movie reader
  src\nes\             hardware layer: PPU, AxROM mapper, controllers, APU register sink
  src\platform\        SDL window, input, audio out, config, paths (M2)
  src\game\            hand-written readable C that replaces generated routines one by one
  tests\cpu\           nestest CPU check
  tests\lockstep\      run interpreter vs recompiled/hand-written code, compare RAM every frame
  tools\               ramdiff.py; fceux\*.lua (RAM dump, write/exec logging for FCEUX); release_audit.py + pre-commit hook
  third_party\         notes on external libraries (fetched, not vendored)
  assets\              gitignored; anything extracted from the user's ROM
```
