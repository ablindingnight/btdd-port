# M4: `btdd.meta` (2026-11-23)

## What exists
| file | what |
|---|---|
| `meta/FORMAT.md` | format v1: line records, `key=value` fields, keyed to the ROM SHA-1, readers skip unknown records/keys |
| `meta/btdd.meta` | the exported metadata (committed, audit-clean) |
| `tools/export_meta.py` | builds it from a decomp commit (`git show`, never the working tree), the ROM and the merged CDL logs; validates |
| `tools/meta_check.py` | loads meta + ROM (no decomp), re-derives facts and prints per-bank stats: the round-trip proof |
| `tools/m6502.py` | official 6502 opcode table shared by both tools |

Run: `python tools/export_meta.py` (about 30 s), then `python tools/meta_check.py` and
`python tools/release_audit.py meta`.

## Format decisions
- **One record per line, every bank record carries its bank**, sorted by (bank, address, type): diffs of a new
  export are a handful of lines, `grep '^farcall 3'` works, no nesting to parse.
- **Positional core + `key=value` extras.** Readers ignore unknown keys and record types, so the exporter can grow
  without a version bump; a change of meaning bumps `btdd.meta <n>`.
- **Ranges tile each bank** (`code` / `data` / `unknown`), so "unknown bytes" is a first-class answer, and the
  recompiler knows exactly where it must fall back to the interpreter.
- **Provenance on every range**: `src` (who drew the boundary), `cdl=exec/len` (what the logs saw), `conf`
  (verified / static / suspect), `naming` (reviewed / auto, with `proposed=` for the model's unreviewed name).
- **No contents.** Data ranges carry a kind, never bytes. The only values taken from the ROM are addresses
  (vector, dispatch-slot and jump-table targets), and the recompiler re-reads and cross-checks them.
- **Code ranges are validated against the ROM, not the source text**: the exporter decodes each instruction the
  source names and fails if the opcode disagrees; a range must disassemble straight to its end. The decomp writes
  some instructions as `.byte` when they double as a table (`sub_84c7` ends with a `jmp` that is also read as 3
  data bytes): those get `notes=code_read_as_data` instead of being treated as data.
- The vectors (`$FFFA-$FFFF`) are split off as `data kind=vectors` so the reset stub is a clean code range.

## Committing `meta/btdd.meta`: yes
It contains addresses, names, kinds and counts only. `python tools/release_audit.py meta tools` = CLEAN
(no 16-byte ROM runs, no hex-spelled ROM runs, no ROM strings), and the pre-commit hook passed on it.
It changes whenever it is re-exported (decomp commit in the `source` line, CDL counts), which is the intent:
the shipped exe embeds the committed version.

## Stats (meta/btdd.meta as committed: decomp 77e9d47be608, 17 CDL logs)
| bank | routines | verified | entries | code | data | unknown |
|---|---|---|---|---|---|---|
| 0 | 553 | 479 | 373 | 29234 | 1181 | 2353 |
| 1 | 25 | 23 | 4 | 779 | 31729 | 260 |
| 2 | 43 | 41 | 7 | 1769 | 28541 | 2458 |
| 3 | 105 | 87 | 34 | 3368 | 26923 | 2477 |
| 4 | 59 | 56 | 28 | 3540 | 27526 | 1702 |
| 5 | 99 | 83 | 67 | 6262 | 24679 | 1827 |
| 6 | 233 | 223 | 75 | 13063 | 15858 | 3847 |
| 7 | 26 | 20 | 4 | 690 | 32006 | 72 |
| all | 1143 | 1012 | 592 | 58705 (22.4%) | 188443 (71.9%) | 14996 (5.7%) |

- Routine ends: 518 jmp, 414 rts, 14 rti, 21 jmp (ptr), 28 always-taken branch, 82 fall into the next routine,
  **66 open** (62 into unknown bytes, 4 into data).
- 105 dispatch slots, 77 far calls (71 resolved, 6 dynamic), 2 xycalls, 9 jump tables (61 targets; 5 strong),
  24 `jmp (ptr)` sites of which 8 have no known targets.
- Data kinds: table 37.8 KB, music 8.4 KB, graphics 1.3 KB, ptrtable 0.7 KB, text 0.4 KB, unknown 139.8 KB.
- 92 runs (1173 bytes) that the logs saw executed lie outside carved code (`xcode`); 42 `xref`s point into
  uncarved bytes.
- Timing: 1 PCM writer (bank 3 `$89AC`), 3 OAM DMA, 69 bank-switch stores, 13 PPUSTATUS polls (10 sprite-0,
  3 vblank), 9 delay loops. No RAM spin loops: the game does its frame work from NMI.
- RAM: 159 records (reviewed from `ram.inc`, proposed from `RAM_PROPOSALS.md`).

## What M5 still needs from the decomp (and from us)
1. **Carve the 66 open-ended routines' fall-through code** (`grep 'end=open' meta/btdd.meta`): mostly bank 0
   `loc_*` fragments that stop after a conditional branch whose other side was never executed.
2. **Carve the 92 `xcode` runs and 42 `xref` targets**, notably the `$FF00-$FFFF` stub area (IRQ/NMI helpers,
   the `$FFCB` trampoline is data in banks 3 and 7) and `jlist` slots that point into unknown bytes
   (bank 0 `$8319`, `$8F68`; bank 1 `$F4AB`; bank 3 `$8962`; bank 6 `$A4F6`).
3. **Targets of the 8 unknown `jmp (ptr)` sites**: bank 0 `$FDFD`/`$FE20` (`jmp ($2A)` after `txs` in the NMI
   path: a saved continuation), bank 3 `$89A6`, bank 5 `$80B8`/`$8127` (computed `$812A + 4n`), bank 6 `$80AA`,
   `$9992`, `$9995`. Best source: a run-time log of (site, target) pairs from the coverage recorder's
   instruction hook over the TAS movies. Until then the runtime dispatches them via the address -> function map.
4. **The 6 dynamic far calls** (X or Y not an immediate): bank 0 `$84C4`, `$9F6C`; bank 2 `$FD49`; bank 4
   `$ABB9`, `$C599`; bank 6 `$E7ED`. Same logging fix, or a per-site note of the possible banks.
5. **Data kinds**: 140 KB of data is still `unknown` kind (likely CHR tiles for CHR-RAM uploads and level data);
   needed for HD packs / mods, not for M5 correctness.
6. Weak jump tables (`conf=weak`, pointer `$06A9`/`$06B2`/`$1F`) need a human look.
7. Exporter is deterministic for a given decomp commit + CDL folder (re-export of decomp 8d0ba209aa74 with 35 logs
   differs from the committed file in 3 lines). The coverage session adds `auto-*.cdl`
   logs over time, which raises the `cdl=` counts in the next export.
