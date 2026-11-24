# M6: per-routine decompile harness (2026-11-24)

Goal: the local model (Qwen3.6-35B-A3B on llama-server) turns one 6502 routine at a time into readable C, and a
differential test decides automatically whether the C is equivalent. Passing C is the **private** decompile: it
is written to `private/m6/` (gitignored) and never committed or shipped. Only the harness code is in the repo.

## Pieces (`tools/m6/`)
| file | role |
|---|---|
| `m6_pick.py` | ranks routines: leaf code (`conf=verified`, ends in RTS, no JSR / jump out / indirect jump) by TAS call count (decomp `docs/STAGE_MAP.tsv` via `git show`); `--list` writes the capture list |
| `m6_capture.c` | replays a TAS on the reference interpreter; per listed routine keeps up to N distinct entry states (registers, PRG bank, 2 KB RAM) with every hardware access until the routine's own RTS; samples cut by an NMI or a stack unwind are dropped; reservoir sampling over the whole movie |
| `m6_corpus.h` | the corpus format (`..\Gamefiles\Movies\m6\corpus\{2p,1p}\b<bank>_<addr>.m6c`, ROM/RAM-derived: outside the repo) |
| `m6_api.h` | what a candidate may use: `void routine(M6 *m)`, registers/flags in `m`, `RAM(addr)`, `bus_read`/`bus_write`; RAM names from the meta as `#define`s (`m6_names.h`, generated) |
| `m6_harness.c` | per sample: runs the original code in a sandbox (RAM + mapped ROM bank, recorded hardware reads replayed) and checks it reproduces the live run; runs the candidate from the same state; compares RAM (except the free stack area), the ordered hardware accesses, and the declared output registers; prints `PASS n/n` or one compact `FAIL state ...` line |
| `m6_test.py` | `m6_test.py BANK:ADDR candidate.c [--outputs a,x,c]`: compiles harness + candidate (zig cc) and runs both movies' corpora |
| `m6_prompt.py` | builds the prompt: fixed instructions + API first (prefix-cache friendly), then routine facts, RAM names used, ROM addresses read, the decomp's asm (`git show HEAD:src/bankN.s`), the M5 mechanical C with the bookkeeping stripped, and the outputs the test compares |
| `m6_run.py` | driver: prompt -> model (thinking on; temperature 0.2, then 0.7 on retries; max 3 tries) -> extract the last ```c block -> test -> on failure append the FAIL line and retry; logs every try to `..\Gamefiles\Movies\m6\runs.jsonl` |

Outputs (registers the test compares): A/X/Y if the routine writes them, C if it ends in `CLC`/`SEC; RTS`
(a status return). Flags otherwise are not compared: callers rarely use them after an RTS, and demanding them would
reject readable C for no gain; when a caller does depend on one, add it with `--outputs`.

## Corpus (built 2026-11-24 for the 40 top leaf routines)
`m6_capture` on both movies (2P old PPU 47 s, 1P new PPU 114 s): 2,000 samples per routine per movie for 39 of 40
routines (`sub_a3bc`, a PPUSTATUS wait, has 216: most of its calls are interrupted by the NMI it waits for).
157 MB + 94 MB. **Every sample of all 40 routines replays exactly in the sandbox** (`M6_VALIDATE=1` mode:
0 unusable), so a failure is always the candidate's.

Harness check: a hand-written `sub_8624` (the game's RNG step) passes 4,000/4,000; the same with one constant
changed fails at state 1 with `RAM $002C expected $A4 got $A5`.

## Model run
See the section added after the run (pass rate, tries, tokens and time per routine).

## Not done / next
- `facts.py` output is not in the prompt: running decomp tools needs the user's OK (AGENTS.md); the prompt uses
  the meta's facts (names, RAM map, call counts) instead.
- Non-leaf routines: the callee interface (a C prototype per callee, with the corpus recording calls as events) is
  the next step; leaf routines prove the loop first.
- Cycle mode: the harness is cycle-free; candidates that must keep timing (PCM, raster waits) stay asm.
