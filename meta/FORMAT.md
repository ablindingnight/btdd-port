# `btdd.meta` format, version 1

`btdd.meta` tells the load-time recompiler (M5) how to read the player's own ROM: where the routines are,
how control moves between them, which bytes are data, what the RAM means, and which spots are timing-sensitive.
It holds **addresses, names and classifications only**. It never holds instruction listings, table contents,
text, graphics or music. It is built by `tools/export_meta.py` from the private decomp and checked against a ROM
by `tools/meta_check.py`.

## Design rules
- **Text, one record per line**, UTF-8, LF. Fields are separated by single spaces. No field contains a space.
- **Self-contained lines.** Every bank-level record carries its bank number, so a line can be grepped, sorted
  and diffed on its own. The exporter sorts records by (bank, address, record type) so that a change in the
  decomp shows up as a few changed lines.
- **Positional fields first, then `key=value` fields.** Readers must ignore keys they do not know.
- **Unknown record types are skipped** (and counted) by readers. Adding a record type or a key does not change
  the version. Changing the meaning of an existing field does: bump the number in the first line.
- `#` starts a comment line (the exporter writes none; hand-written override files may).
- Addresses are `$` + 4 upper-case hex digits (`$8033`). Zero-page pointers in `ptr=` fields may be 2 digits.
  Bank-level addresses are CPU addresses `$8000-$FFFF` inside that bank (AxROM maps the whole 32 KB bank there).
- Names are C identifiers. A name is unique within its bank (`code`, `data`, `entry` names share one namespace).
  The same name may appear in several banks (`nmi`, `irq`, `reset`, `vectors`): generated C prefixes the bank,
  e.g. `b3_nmi`.

## Header records (in this order, before everything else)
| record | example | meaning |
|---|---|---|
| `btdd.meta <version>` | `btdd.meta 1` | must be the first line |
| `rom` | `rom sha1=0218a66c... prg=262144 mapper=7 banks=8` | the only ROM this file describes; a reader refuses any other ROM |
| `source` | `source decomp=77e9d47be608 exporter=tools/export_meta.py cdl_logs=17` | provenance: decomp commit, number of code/data logs merged |
| `conv` | see below | calling conventions the recompiler must know |

### `conv` records
- `conv farcall stub=$FFCB x=slot_lo y=bank_index bank_table=$FFB3 ptr=$13 slot=$8000+x`
  Cross-bank call: `ldx #lo / ldy #i / jsr $FFCB` runs the dispatch slot `$80lo` in bank `byte[$FFB3+i]`
  (the bank number is read from the player's ROM at that address). Every bank starts with `jmp ($13)` at `$8000`
  (the stub stores the slot address in `$13`) followed by a list of `jmp $xxxx` slots (`jlist` records).
  The recompiler should turn a resolved `farcall` into a direct call of the target function.
- `conv xycall bank=0 stub=$85CF x=lo y=hi`: bank 0 helper that jumps to `$hilo` (`xycall` records).
- `conv entity_slots count=15 slot0=player1 slot1=player2`: entity arrays are 15 bytes, one per slot.

## Range records: each bank is tiled exactly by `code`, `data` and `unknown`
```
code    <bank> <start> <end> <name> naming=<n> end=<e> src=<s> cdl=<exec>/<len> conf=<c> [proposed=<name>] [notes=<list>]
data    <bank> <start> <end> <name> kind=<k> naming=<n> src=<s> cdl_read=<r>/<len> cdl_exec=<x> [proposed=<name>]
unknown <bank> <start> <end> cdl_exec=<x> cdl_read=<r>
```
`<end>` is inclusive. Together the ranges cover `$8000-$FFFF` of every bank with no gap and no overlap.

- `code`: one routine (or routine fragment) from the decomp.
  - `naming`: `reviewed` = a meaningful name that went through review in the decomp; `auto` = address-derived
    (`sub_xxxx`, `loc_xxxx`); `proposed=` carries an unreviewed model proposal when the name is still auto.
  - `end`: how the range ends, from straight disassembly of the ROM: `rts`, `rti`, `jmp` (absolute),
    `jmpind` (`jmp (ptr)`), `bra` (a conditional branch that is always taken: `clc/bcc`, `lda #0/beq`,
    `beq x/bne y` ...), `brk`, `fall` (no terminator but the next range is code that starts right after it:
    execution falls through into the next routine), `open_into_unknown` / `open_into_data` (the last
    instruction can fall through into bytes the decomp has not carved: the recompiler must fall back to the
    interpreter there).
  - `src`: `carve` (range header from the decomp's static carver), `label` (no header; extent = up to the next
    range), `port` (our override file).
  - `cdl`: bytes of the range that the merged code/data logs saw executed / range length.
  - `conf`: `verified` = entry seen executed in a log and the range ends properly; `static` = ends properly but
    never seen executed; `suspect` = open end.
  - `notes`: `code_read_as_data` (some instruction bytes are also read as a table, e.g. a trailing `jmp` used as
    3 data bytes); `embedded_data` (the range holds non-instruction bytes; it then need not disassemble straight).
- `data`: bytes read as data. `kind`: `table` (indexed by code with an absolute address), `ptrtable` (pairs of
  bytes loaded into a pointer: `lda T,x / sta p / lda T+1,x / sta p+1`), `text`, `graphics`, `music`, `vectors`
  (`$FFFA-$FFFF`), `unknown`. Contents are never included: the recompiler reads them from the ROM.
- `unknown`: not classified by the decomp yet. `cdl_exec` > 0 means some of it is code (see `xcode`).

## Control-flow records
```
vec      <bank> nmi|reset|irq <target> <name>
entry    <bank> <addr> <name> in=<routine> via=<how,...>
xcode    <bank> <start> <end> in=<range name> whole=yes|no
jlist    <bank> <slot> <target> <target name>
indirect <bank> <site> ptr=<p> kind=dispatch targets=jlist
indirect <bank> <site> ptr=<p> tables=<table,...>
indirect <bank> <site> ptr=<p> targets=unknown
jtable   <bank> <table> count=<n> ptr=<p> site=<load site> jmp=<jmp (p) site> conf=strong|weak
jtarget  <bank> <table> <index> <target> <target name>
farcall  <bank> <site> bank=<b> slot=<slot> target=<addr> <target name> [tail]
farcall  <bank> <site> bank=? slot=? target=? dynamic [x=$nn] [y=$nn] [tail]
xycall   <bank> <site> target=<addr> <name>  |  xycall <bank> <site> target=? dynamic
xref     <bank> <target> unknown|data|midinsn from=<site> in=<range name>
```
- `vec`: the CPU vectors of each bank (each AxROM bank has its own).
- `entry`: an address inside a code range that is entered from elsewhere (`jsr@$xxxx`, `jmp@...`, a branch
  from another routine, a `jtable`, an `xycall`, or a label in the decomp). The recompiler must make each one a
  callable/jumpable point (a separate C function or a labelled entry into the containing one).
- `xcode`: bytes inside a `data`/`unknown` range that the code/data logs saw **executed**. `whole=yes` when the
  run disassembles to whole documented instructions. Not yet carved by the decomp; interpreter territory.
- `jlist`: the dispatch slots after `jmp ($13)` at `$8000`. `indirect ... kind=dispatch` is that `jmp ($13)`.
- `jtable`/`jtarget`: a table of 16-bit code addresses used by a `jmp (ptr)`. `conf=strong` when the loads and the
  `jmp (ptr)` are in the same routine, `weak` when only the pointer matches.
- `indirect ... targets=unknown`: a `jmp (ptr)` whose targets the static pass could not find (a computed address
  or a saved continuation). The recompiler must dispatch it at run time through its address -> function map.
- `farcall`: a resolved cross-bank call site (`tail` = reached by `jmp $FFCB` instead of `jsr`), or a dynamic one
  where X or Y is not an immediate right before the call.
- `xref`: a `jsr`/`jmp`/branch into bytes that are not carved code (or into the middle of an instruction).

**Values read from the ROM.** `vec`, `jlist` and `jtarget` targets are addresses (facts about where code is),
recorded so a reader can see the call graph without the ROM. The recompiler re-reads them from the player's ROM
and treats a mismatch as an error (`tools/meta_check.py` does exactly this).

## Timing records
```
timing <bank> <addr> pcm_write reg=$4011
timing <bank> <addr> oam_dma reg=$4014 stall=513
timing <bank> <addr> bank_switch store=<addr> mode=<addressing mode>
timing <bank> <addr> ppustatus_poll bit=vblank|sprite0|overflow|mask_unknown loop=<branch addr>
timing <bank> <addr> delay_loop loop=<branch addr>
timing <bank> <addr> idle_loop
timing <bank> <addr> ram_spin addr=<ram> loop=<branch addr>
```
`<addr>` is the instruction address (the store itself; emulator logs that print the PC after the instruction
show it 3 bytes later, e.g. the `$4011` writer logged at `$89AF` is the store at `$89AC`). Generated code must
count CPU cycles up to each of these so the hardware layer can timestamp the access (PCM, sprite-0 splits),
must model the DMA stall, and must re-enter the bank map after a `bank_switch` store.

## RAM records (after all bank records)
```
ram <addr> <size> <name> kind=var|ptr|array|alias|io naming=reviewed|proposed [stride=1 index=slot|player] [of=<name> part=hi]
```
- `array index=slot`: one byte per entity slot (`count` from `conv entity_slots`); `index=player`: 2 bytes.
- `ptr`: 2-byte little-endian pointer; `alias ... part=hi` names its high byte.
- `io`: NES hardware registers (standard names).
- `naming=proposed` names are hints; they may repeat and may be wrong.

## Overrides
`meta/overrides.meta` (optional, hand-written, our own findings) uses the same syntax for bank-level records. The
exporter appends each line with `prov=port` unless it already has a `prov=`.

## Example
```
btdd.meta 1
rom sha1=0218a66c6ee9887ec9115213a2f5b64055d79599 prg=262144 mapper=7 banks=8
code 0 $8000 $8002 sub_8000 naming=auto end=jmpind src=carve cdl=3/3 conf=verified
indirect 0 $8000 ptr=$13 kind=dispatch targets=jlist
jlist 0 $8012 $8033 sub_8033
farcall 0 $8335 bank=6 slot=$8006 target=$A24E sub_a24e
timing 3 $89AC pcm_write reg=$4011
ram $0485 15 ent_pos_x kind=array naming=reviewed stride=1 index=slot
```
