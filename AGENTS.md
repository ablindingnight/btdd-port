# Rules for agents (read first)

- Before starting work, read `C:\Users\theop\claude\Battle\STATUS.md` (live progress, gaps, who works where; owned
  by the coordinator session: send it updates instead of editing it). Game facts: `docs/GAME_FACTS.md`.

- This repo is the PC port. The decomp at `..\btdd-decomp` is READ-ONLY from here: never edit, commit, or run its
  loop from this repo. Ask the user before running anything in it (its Kilo loop may be running).
- ROM: `..\Gamefiles\Battletoads-Double Dragon (USA).nes`. Never copy it, dump banks into this folder, or commit
  anything derived from ROM bytes (docs/ROM_POLICY.md). `generated/` and `assets/` are gitignored for that reason.
- The pre-commit hook (`tools/pre-commit-hook.sh`, installed in `.git/hooks`) runs `tools/release_audit.py`:
  it blocks commits with ROM byte runs, ROM bytes spelled as hex, ROM strings, or files under private/,
  generated/, assets/. Never bypass it. Run `python tools/release_audit.py <folder>` on any release folder too.
- Never hand-edit `generated/`; change `recomp/` and regenerate.
- A change to `src/game` or `recomp/` is done only when the lockstep test is clean on the agreed movie range.
- FCEUX and Mesen may be opened anytime without asking (user, 2026-11-23); close their windows when done.
  The headless core (`btdd_headless`) is preferred: it matches FCEUX old-PPU RAM on every frame of the 2P TAS.
- Build: CMake from Visual Studio 18 (see README). Keep C portable (C11, no MSVC-only extensions outside `src/platform`).
