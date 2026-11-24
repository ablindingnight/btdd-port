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
  3. the last answer passed all 4,000 corpus states and failed random state 3 with "read of `$3FBD` where the
     original's next access was `$3EBD`". **That was a harness artifact, not a model bug**: the random state put
     `ptr_1f` into PPU register space, and a page-crossing `lda (ptr),y` makes a 6502 *dummy read* at the
     unfixed address (`$3EBD`) before the real one (`$3FBD`). C cannot express that read, and the game never
     does it (this routine's corpus has no hardware access at all; the pointer always points at ROM tables).
     The harness now skips random states that touch a hardware address the corpus never shows the routine
     touching; with that, this answer passes (4,000 corpus + 1,272 random states, 728 skipped). It is in
     `private/m6/` as passing (the run log still shows it as failed, since it was re-tested after the run).
  4. it used `ptr_1f_hi`, which `m6_names.h` lacked (aliases were skipped): all RAM-map names are included now.
- Readability: the passing C is correct but mostly instruction-by-instruction with explicit flag updates. A second,
  "simplify" pass (same test as the gate) is the cheap next step once correctness is in hand.

## Batch run, 2026-11-24 01:39-03:15 (serial requests, interleaved with the decomp loop)
Leaf routines from the top-100 list (call-graph mode never started: the leaf list was not finished by 03:15).
All runs of the night together (`python tools/m6/m6_stats.py --since 2026-11-24`), corrected for `sub_d73f`
(passes after the harness fix) and `sub_ef4a` (stopped mid-try at 03:15, not counted):

| | routines | share |
|---|---|---|
| tried to the end | 42 | |
| pass on try 1 | 21 | 50% |
| pass after retries (try 2-3) | 16 | 38% |
| **pass total** | **37** | **88%** |
| fail after 3 tries | 5 (`sub_8d5b`, `sub_8ec8`, `sub_a262`, `sub_a277`, `sub_b4c3`) | 12% |

- Mean 1.7 tries, **~19k completion tokens and ~141 s of model time per routine** (median 98 s; easy ones
  20-60 s, failures 4-7 min). Prompts 1-4k tokens; nearly all tokens are thinking.
- Failed tries by kind (all 49 failed tries): RAM value wrong (arithmetic, indexing, loop bounds) 17; does not
  compile (undeclared variables, labels from the mechanical C) 8; flag outputs (mostly carry that callers test)
  6; register outputs 4; no answer within the 32k-token budget 3; plus one endless loop (8-bit counter `>= 0`).
  Carry is the most common first-try mistake on otherwise right code; retries with the FAIL line fix it.
- **Projection**: ~1,143 routines x ~141 s = **~45 h of model time** on one llama-server slot, serial, at this
  rate. That is a lower bound: these were the smallest, most-called leaf routines (6-89 bytes). Bigger routines
  cost more tokens and fail more often; far calls, indirect jumps and timing-critical code are not handled yet.
  With the slot to itself overnight (no decomp loop) and the fail rate at ~12%, a few nights cover the routines
  the harness can take today.

## M6 at scale: what it would take (STATUS.md gap #1)
Routine sizes over the whole meta: 1,143 routines, median 22 bytes, mean 51, 90th percentile 140, largest 650. The
42 routines tried tonight averaged ~30 bytes, so they are representative of the median but not of the tail.

**Time budget.** Measured: ~141 s and ~19k completion tokens per routine at ~30 bytes, 88% passing within 3 tries.
Model time grows roughly with the amount of code; the 89-byte routines took 2.5-7 min. Estimate for all 1,143:
**~70-90 h of model time** (about 2x the 45 h straight-line projection, for the long tail and retries), i.e. 7-9
nights of 10 h on one dedicated slot, or about half that with two llama-server slots (`-np 2`) if VRAM allows.
Corpus: 2,000 samples x 2 movies is ~4.5 MB per routine (531 MB for 119 routines now); for all routines use 500
per movie (~1.5 GB, captured in a few minutes). The 65 `static` routines never ran in the TAS: test them on random
states only until human play (STATUS gap #4) gives them a corpus.

**Non-leaf strategy** (in this order, each a harness feature with the same test as the gate):
1. *Stub callees instead of waiting for them* (small change): let a routine go as soon as its callees are
   *known*, linking passing C where it exists and the interpreter stub (`m6_call`) otherwise. Call-graph order then
   only improves the prompt (callee C shown), it no longer blocks; today only 18 non-leaf routines become ready.
2. *Far calls* (`ldx #lo / ldy #i / jsr $FFCB`): an API call `far_call(m, i, lo)` whose harness side runs the
   original trampoline on the interpreter (exact bank write and stack), so the C never re-implements the stub.
3. *Jump tables / `jmp (ptr)`*: `dispatch(m, ptr_addr)` that calls the target routine (C if passing, else
   interpreter); the meta's `jtable`/`indirect` records give the target sets for the prompt.
4. *Tail jumps out of the range* (`jmp other_routine`): a call followed by return; *alternate entries* (`entry`
   records inside a routine): one C function per entry, sharing a static helper.
5. Leave in asm (interpreter) for now: NMI/IRQ handlers, stack tricks (`pla/pla`, `txs`), the PCM writer and
   raster waits (timing), code in RAM.

**Readability review** (correctness alone gives C that is instruction-by-instruction with flag bookkeeping):
1. A *simplify pass* per passing routine: prompt = passing C + names + "rewrite for a reader: loops instead of
   gotos, no flag variables except the outputs, RAM names instead of addresses, comments in game terms"; the
   result must pass the same corpus + random-state test, else the previous version stays.
2. A cheap *lint score* to rank what needs review: raw `RAM(0x..)` where a name exists, `goto` count, flag
   assignments that are not outputs, lines per instruction. Re-run simplify on the worst; spot-check ~10% by
   Claude or a human.
3. *Names first* (STATUS gap #2): grade-and-apply the name proposals and add struct views of the entity arrays
   before the bulk run, so the C reads `ent[x].hp` instead of `RAM(0x051B + x)`; renaming after the fact means
   re-running simplify.

## Stub callees (2026-11-24 ~04:00)
`m6_run.py --callgraph --stub-callees`: a non-leaf routine no longer waits for its callees; callees without
passing C run as the original code through the interpreter stub (this also covers far calls through the `$FFCB`
trampoline, itself a routine start). Ready set with 37 passing routines: 8 -> 80. The prompt shows each callee's
C, or, for a stubbed one, its 6502 listing (what it reads and writes). First 5: **all pass, all on try 1**
(`sub_8261`, `sub_a423`, `sub_dc11`, `sub_f1d8` with a far call, `sub_9160`); `sub_9160` failed 3 tries until
the stubbed callees' listings were added to the prompt, then passed at once. Prompts also carry the decomp's
`facts.py` output (run read-only as `python ../btdd-decomp/tools/facts.py BANK LABEL`, allowed by the user's
local settings).

## Token budget (2026-11-24 ~04:30)
llama-server honours `thinking_budget_tokens` per request (probe: 1,727 -> 237 generated tokens at a 200 budget,
same answer; `reasoning_budget` and a template kwarg had no effect). The driver now caps the first try at 16k tokens
with a 12k thinking budget, retries after a wrong answer at 24k / 20k, and after a cap hit retries once with
thinking off (6k) on the prompt without the mechanical C. Re-run of 7 routines (completion tokens, before -> after):
`read_controllers` 14.1k -> 8.7k pass, `sub_dc11` 15.9k -> 13.3k pass, `sub_9160` 16.6k -> 12.9k pass,
`sub_8624` 7-10k try 1 -> 34k pass on try 3, `sub_8ec8` 52k no answer -> 32k wrong answers, `sub_8609` 14.2k pass
-> 34k fail, `sub_a305` 19.3k pass -> 40k fail. No try hit the cap any more (max ~22k on retries vs 32k before).
The two regressions did not come back with the larger retry budget; `sub_8609` passed again on try 1 without the
facts.py block (`M6_NO_FACTS=1`), `sub_a305` failed with and without it. With samples this small the run-to-run
variance at temperature 0.2-0.7 dominates; a fair comparison needs the same 20+ routines run 2-3 times per setting.

## A/B of the token settings (2026-11-24 05:00-07:35)
The same 12 routines (`0:8624 0:8609 0:8dd2 0:dc11 5:a305 3:8ec8 3:8c4a 0:aa0e 5:b151 0:8d5b 6:a262 0:9160`), up to 3
tries each, serial, one setting after the other (`M6_TAG`, `m6_stats.py --tag`):

| setting | try-1 passes | passed | completion tokens / routine | model s / routine | passes per GPU-hour |
|---|---|---|---|---|---|
| 16k cap + 12k thinking budget, facts.py block | 4 | 7/12 | 28.6k | 229 | 9.2 |
| 16k cap + 12k thinking budget, no facts | 3 | 8/12 | 27.6k | 212 | **11.3** |
| legacy: 32k cap, no budget, no facts | 1 | 7/12 | 34.3k | 267 | 7.9 |

The budget saves ~20% tokens and time per routine for the same or better pass rate, and gets more first-try passes.
The facts.py block did not help (one pass fewer, within noise) and costs a subprocess plus prompt tokens, so it is
now off by default (`M6_FACTS=1` turns it on). Only `sub_dc11`, `sub_a262` and `sub_b151` passed in all three runs and
`sub_8ec8` failed in all three: per-routine outcome is still mostly variance; 12 routines separate settings only
coarsely. Failure kinds are the same in every setting: RAM values (arithmetic/indexing) first, then flag outputs.

## Non-leaf routines (branch m6-nonleaf, 2026-11-24 afternoon)
`m6_pick.classify` names every way out of a routine besides its own RTS and gives the routine the hardest kind it
needs: `leaf`, `calls` (JSR to a routine start of the same bank), `tail` (JMP/branch to another routine's start, or
running on into the next code), `alt` (JSR/JMP into a meta `entry` inside another routine), `far` (`jsr $FFCB`),
`dispatch` (`jmp (ptr)`). Not supported: BRK/RTI, TXS, code in RAM, jumps to unknown code, the trampolines
themselves. 991 of 1,143 meta routines classify (leaf 213, calls 80, tail 277, alt 372, far 37, dispatch 12).
- C API (`m6_api.h`): `far_call(m, bank_index, slot)` runs the original `$FFCB` trampoline (exact bank writes);
  `jump_to(m, target)` runs a `jmp (ptr)` target at the routine's own stack level (a JMP, not a JSR: exact for
  random states too). Tail jumps are `name(m); return;`; entries are callee functions named after the meta
  `entry` record. Hand-written candidates for `sub_8638` (far) and `sub_8774` (dispatch) pass; a wrong far-call
  slot fails on the bank write.
- Prompt: the mechanical C shows exits as these calls; far-call sites list their targets
  (`coverage/indirect_targets.tsv`), `jmp (ptr)` sites the targets seen in play.
- RAM names come from the decomp's HEAD (`src/ram.inc` + `docs/RAM_PROPOSALS.md`, read with `git show` through the
  meta exporter's `build_ram`): 285 names vs 156 in `btdd.meta` (129 new addresses, 23 renamed, e.g. `$8D`
  `tmp_8d` -> `camera_x_lo`); older names stay as aliases. Each compile unit gets its own names header without the
  names it declares as locals. `m6_recheck.py` re-tests every passing file: 109/109 pass after the changes.
- `m6_run --kinds K,... --new-only --shard i/n`: best-first by share of callees with passing C, then calls;
  `m6_stats --by-kind`. Corpus captured for 697 non-leaf routines (500 samples per movie); 301 files came out empty
  (never run in that movie) and are now skipped.
- Batch 12:13-15:10 (tag `batch_nonleaf`, untested-corpus routines left out): 105 routines, 79 passed (75%), 66 on
  try 1, 17.9 passes per GPU-hour, mean 16.6k tokens / 152 s per routine (median 75 s).

| kind | routines | try 1 | passed | pass rate |
|---|---|---|---|---|
| calls | 1 | 1 | 1 | 100% |
| tail | 28 | 20 | 22 | 79% |
| alt | 71 | 43 | 53 | 75% |
| far | 3 | 1 | 1 | 33% |
| dispatch | 2 | 1 | 2 | 100% |

- One vs two llama-server slots: one process 12:13-13:19, 37.8 routines/h (34.0 passes/h); two processes
  (`--shard 0/2`, `1/2`) 13:20-15:10, 31.6 routines/h (22.7 passes/h). Per request generation fell from 135 to
  102 tokens/s, so the two slots together produce ~1.5x the tokens, but the later (lower-ranked) routines needed
  46% more tokens per try and 19 empty-corpus routines were sent before that fix: the comparison is not like for
  like. Keep two processes; compare on the same routine set to measure the real gain.
- 187 routines have passing C now (private/m6, never committed).

## Not done / next
- `facts.py` output: allowed and wired in (`M6_FACTS=1`), off by default after the A/B above.
- Non-leaf routines: done for same-bank callees (below). Far calls (`$FFCB`), indirect jumps and tail jumps out
  of the routine are not handled yet, which leaves few non-leaf routines ready: with 100 leaf routines passing,
  only 18 more become ready in call-graph order.

## Call-graph order (added 2026-11-24 ~01:50)
- A JSR becomes a C call `sub_xxxx(m)` with the same register/flag contract as the 6502 subroutine. `m6_test.py`
  links each callee's passing C from `private/m6/` (renamed from `routine`), or, for a callee without one, a stub
  that runs the original callee on the interpreter against the same RAM and recorded hardware (`m6_call`).
  Candidates may not call `m6_call` themselves (that would just run the original code).
- `m6_pick.callgraph_ready()` lists non-leaf routines whose callees all have passing C; `m6_run.py --callgraph`
  works through them, re-ranking after each pass. The prompt lists each callee with the outputs its callers use
  and its C.
- A candidate that never returns (seen on the first batch: an 8-bit loop counter compared with `>= 0`) is killed
  after 120 s and reported as a FAIL naming the state it hung on; batches log unexpected errors and go on.
- Cycle mode: the harness is cycle-free; candidates that must keep timing (PCM, raster waits) stay asm.
