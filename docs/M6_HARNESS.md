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

Outputs (registers/flags the test compares besides RAM and hardware accesses): those some caller reads after the
`JSR` before overwriting them, found by a bounded path search from every call site in the bank (both branch
directions, `jmp`, into callees until their RTS). Leaving known code (the caller's own RTS, an indirect jump)
counts as a read of the pending registers but not of flags. Routines with no JSR site in their bank (dispatch
slots, far-call targets) fall back to "registers the routine writes". Cross-bank callers are not analysed yet.

Every test also runs 2,000 random entry states (random registers, flags, RAM; hardware reads answer random bytes,
recorded from the original and replayed to the candidate) after the corpus, so a candidate that only matches the
states the TAS happened to produce is still caught.

## Corpus (built 2026-11-24 for the 40 top leaf routines)
`m6_capture` on both movies (2P old PPU 47 s, 1P new PPU 114 s): 2,000 samples per routine per movie for 39 of 40
routines (`sub_a3bc`, a PPUSTATUS wait, has 216: most of its calls are interrupted by the NMI it waits for).
157 MB + 94 MB. **Every sample of all 40 routines replays exactly in the sandbox** (`M6_VALIDATE=1` mode:
0 unusable), so a failure is always the candidate's.

Harness check: a hand-written `sub_8624` (the game's RNG step) passes 4,000/4,000; the same with one constant
changed fails at state 1 with `RAM $002C expected $A4 got $A5`.

## Model run (Qwen3.6-35B-A3B Q4_K_M, llama-server shared with the decomp loop, thinking on), 2026-11-24 00:30-01:15
8 leaf routines tried, **7 pass** (corpus of both movies + 2,000 random states). All passing C is in
`private/m6/` (gitignored).
| routine | size | result | tries | completion tokens | time |
|---|---|---|---|---|---|
| `sub_8624` (RNG step) | 20 B | pass | 1 | 6,973 | 55 s |
| `sub_8550` | 15 B | pass | 1 | 5,851 | 44 s |
| `sub_81eb` | 12 B | pass | 1 | 3,062 | 23 s |
| `sub_83eb` | 15 B | pass | 1 | 4,561 | 39 s |
| `read_controllers` ($4016/$4017 order) | 49 B | pass | 1 | 14,094 | 108 s |
| `sub_a305` (ROM tables) | 89 B | pass | 1 | 19,258 | 160 s |
| `sub_feda` (hardware + delay loop) | 52 B | pass | 3 | 33,933 | 259 s |
| `sub_d73f` (pointer + ROM tables) | 54 B | **fail** | 3 | 78,635 | 673 s |
Prompts are 1.2-4.2k tokens; almost all tokens are thinking. Over all 19 logged tries: 252k completion tokens,
34 minutes of model time.

What the run taught (fixed in the harness/prompt during the night):
- The first `sub_8624` answer cleared C at the end; with outputs taken from the instructions it wrote, the test did
  not compare C and let it pass. Callers do read C, and the caller-liveness outputs now reject it; the second answer
  passes. The first version of `sub_a305`/`sub_d73f` runs failed on extraction (prose taken as code), the 16k
  token cap, and missing declarations; the extractor now takes only a block that defines `void routine(`, the cap is
  32k and the prompt asks for short reasoning.
- `sub_d73f` still fails, but for understood reasons, each now handled:
  1. it ends `lda #$00 / rts` and its caller writes A to the bank register, so A is a real output; the decomp
     source shows those bytes as `.byte $A9` / `brk` (they double as table data) and the model trusted it. The
     prompt now adds an authoritative ROM disassembly whenever the listing contains `.byte`; after that, A is right;
  2. a growing retry chat made the model re-think everything until the 32k cap; retries now start a fresh
     conversation (prompt + last code + its FAIL line);
  3. the last answer passed all 4,000 corpus states and failed random state 3: its `(ptr),y` read hit `$3FBD`
     where the original reads `$3EBD` (page carry of the indirect address). The corpus alone would have accepted
     it; the random states caught it. Not fixed by the model within 3 tries tonight.
  4. it used `ptr_1f_hi`, which `m6_names.h` lacked (aliases were skipped): all RAM-map names are included now.
- Readability: the passing C is correct but mostly instruction-by-instruction with explicit flag updates. A second,
  "simplify" pass (same test as the gate) is the cheap next step once correctness is in hand.

## Not done / next
- `facts.py` output is not in the prompt: running decomp tools needs the user's OK (AGENTS.md); the prompt uses
  the meta's facts (names, RAM map, call counts) instead.
- Non-leaf routines: the callee interface (a C prototype per callee, with the corpus recording calls as events) is
  the next step; leaf routines prove the loop first.
- Cycle mode: the harness is cycle-free; candidates that must keep timing (PCM, raster waits) stay asm.
