# Coverage plan: find the rest of the code and data without FCEUX replays (2026-11-23)

## Why change approach
So far every new byte of known code/data came from emulator logs (FCEUX TAS replays, Mesen play sessions, Lua
hooks). That hit limits: the 1P TAS desyncs (~40k), FCEUX needs a window and babysitting, Lua hooks are slow, and
levels 4+ / 1P / mode B / game over / continue / ending are barely logged. 14,996 bytes are still raw and only 22%
of PRG is carved as code.

## New approach: our own headless emulator does the logging
The M1 reference interpreter (`btdd_headless`) runs thousands of frames per second with no window, and we control
every bus access. Add one feature to it and it replaces FCEUX for all logging:

**Coverage recorder** (`--cdl out.cdl --reads out.tsv`): per PRG byte, flags in FCEUX .cdl format (code, data,
indirect data, bank) so btdd-decomp's existing tools (`cdl.py`, `datacarve.py`, `cdlfrontier.py`) merge it with no
changes; plus a reader table "data byte -> routine PC that read it, stage" (replaces the datastage*.py Lua hooks).

## Ways to reach new code (in order; each one is automatic)
1. **Replay what we have, natively**: 2P TAS (whole game), 1P TAS up to its desync, the koh-*.fm2 user movies.
   Our emulator records a snapshot every 60 frames (in its own state format) for the next steps.
2. **Stage warp + god mode**: level ID is `$000D` and section ID is `$0015` (found 2026-11-23, see
   GAME_FACTS.md; level IDs 0, 1, 7 and sections 0 / >0E never appear in the TAS: warp there first).
   Write it at the stage-load point to start any stage in any setup. Freeze `player_lives` ($11/$12) and `ent_hp`
   ($051B,x) so bots never die. Matrix: stage 1-7 (and any hidden ones) x 1P/2P x mode A/B x each character choice.
3. **Bots**: simple input scripts per stage type (walk right + mash attack, jump on a timer, follow the scroll),
   run in the matrix above. Cheap, deterministic, many runs in parallel (one process per CPU core).
4. **Coverage-guided exploration** (the big one): from every snapshot, run many short random-but-plausible input
   bursts (held directions, attack/jump rhythms, pauses, Start/Select), keep any burst that executes a new code
   byte or reads a new data byte, and branch further from there (AFL-style, using the coverage map as fitness).
   Reaches odd paths: player-vs-player hits, rare enemy states, deaths, continues, menus, pause, glitches.
5. **Forced paths**: set lives to 0 to hit game over / continue; jump to the last stage to reach the ending and
   credits; let the title/demo run for minutes (attract mode).
6. **Static completion**: for still-raw regions, recursive disassembly from jump tables, the `$FFCB` cross-bank
   trampoline table and pointer tables seen in reads; then *probe* each candidate by calling it from a matching
   captured state in the emulator (does it run cleanly to RTS?). Only probed + clean candidates become Carve tasks.
7. **Human play (last resort)**: only for what is still unreached after 1-6, with a short checklist of exactly
   which stage/situation, played in Mesen with the logger on (save under a NEW name in `Gamefiles\Debugger`).

## Progress gates (user note, 2026-11-23)
Some areas only open after a task is done (defeat a wave, hit a trigger, survive a section). Bots and random input
will not pass those, and a stage warp starts at a stage's beginning only. So the TAS movies are the backbone:
- **TAS-anchored exploration**: the TAS route passes every gate, so snapshots are taken densely along it (every
  60 frames, plus at every scroll lock/unlock and stage change) and exploration branches out FROM those points,
  then is allowed to rejoin. Every area the TAS visits becomes a starting point, gates already passed.
- **Gate detection**: while replaying, record when the screen's scroll lock releases or the area/sub-area variable
  changes, and the RAM state just before it. That tells which condition opened the gate (e.g. an enemy counter
  hitting 0) and lets exploration satisfy it (or set it directly, then verify against the TAS behaviour).
- Places the TAS skips (it takes the fastest route, maybe with skips/glitches) are reached by deviating from the
  nearest snapshot and playing normally, or by setting the recorded gate condition. `fceux-glitch.cdl` hints that
  the TAS uses at least one glitch: code seen only there needs a normal-play check too.
- The 1P TAS desyncs around 40k in FCEUX: its snapshots before that point still count. If our core is exact, try
  replaying it with the FCEUX 2.1.0-era timing quirks as an option; otherwise stop at the desync.

## Outputs (all stay out of git where ROM-derived)
- `Gamefiles\Debugger\auto-*.cdl` (merged automatically by btdd-decomp tools) and `auto-reads-*.tsv`.
- A coverage report: % PRG code/data seen, per bank, per source, list of still-unseen regions.
- New Carve / Name tasks via btdd-decomp's `cdlfrontier.py`, `datacarve.py`, `queuegrow.py` for the Qwen loop.
- Bonus for the port: per-routine entry states captured from all of this become the differential-test corpus (M4).

## Prerequisites and order
1. M1 done: interpreter matches FCEUX RAM over the 2P TAS (in progress, `port-m1` session).
2. Coverage recorder + snapshots + reader table (step "Replay" above). Check: replaying the 2P TAS reproduces the
   existing `fceux-tas.cdl` coverage (same bytes flagged), which proves the recorder.
3. Stage variable + warp + god mode, then bots, then exploration. Report coverage after each.
Mesen save states (.mss) are not imported (proprietary compressed format); our own snapshots replace them.
