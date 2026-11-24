# M1: reference interpreter (done 2026-11-23)

## Result
- **2P TAS (tasvideos-4323M, 59,351 frames, the whole game): the 2 KB CPU RAM is identical to FCEUX 2.6.6
  (old PPU) after every single frame.** Checked with `tools/ramdiff.py` on per-frame dumps from both sides.
- nestest: all 5,003 official-opcode lines of the reference log match (PC, A, X, Y, P, SP and cycle count).
- MSVC, clang and gcc builds produce byte-identical dumps; no warnings with any of them.
- Speed: about 4,500 frames/s headless (75x real time) on this machine.
- Second, independent check: the coordinator's earlier FCEUX capture `..\Gamefiles\Movies\musiclog\full\ram.bin`
  (made 2026-11-22 on the old PPU) is also identical on every frame. Its record k (offset k*2048) is the RAM
  after frame k, record 0 being FCEUX's pre-movie frame (`emu.framecount()` = 0), so the header comment
  "frame N at offset N*2048" is right.
- Over the whole TAS: no unofficial opcode, no mapper write whose value differs from the ROM byte under it
  (so AOROM bus conflicts never matter for this game), no DMC sample playback, one `$4015` read.

## What "exact" means here
The TAS movies only sync in FCEUX, and the 2P one was made on FCEUX's **old PPU** (`NewPPU 0` in the fm2
header). So the reference interpreter reproduces FCEUX's old-PPU timing model, not the real console's, where
the two differ. Our code is written from scratch; FCEUX's source (GPL, `..\tools\fceux-src\`, read only) was
used as the specification.

- CPU: each instruction is charged its base cycle count when its opcode is fetched; page-cross and branch
  extras when they happen. Bus accesses see the time after those charges. Dummy reads and writes are done
  where FCEUX does them (they matter for `$2002`/`$2007`/`$4016`).
- Interrupts are polled before each instruction. An NMI enabled through `$2000` while the vblank flag is set
  is taken one instruction late (FCEUX "NMI2"). IRQ polling uses the I flag from before the last
  instruction, except after RTI.
- The CPU runs against a budget in 1/16 PPU dots (48 per CPU cycle); the overshoot of the last instruction
  carries into the next slice.
- Frame schedule (dots): 341 (line 240) -> vblank flag set, OAM address reset -> 12 -> NMI if enabled ->
  20 lines minus 12 -> flag bits 5-7 cleared (pre-render line) -> 256 + 69 -> `v = t` if rendering ->
  16 or 15 (alternating, regardless of rendering) -> 240 visible lines of 256 + 6 + 63 + 16 dots.
  After power-on, two whole frames run with no vblank at all.
- Frame boundary (where fm2 input changes and where RAM is dumped) = start of line 240, like FCEUX's
  `FCEUI_Emulate`.
- Background is drawn lazily per tile, up to the CPU's current dot, whenever a PPU register or the mapper is
  touched; sprite-0 hit is the first opaque sprite-0 pixel over an opaque background pixel among the pixels
  drawn so far (one tile of look-ahead while a hit is pending). Sprite evaluation (8 per line, overflow flag)
  happens at the end of the previous line.
- Power-on state as FCEUX with `RAMInitOption 0`: RAM and CHR-RAM `00 00 00 00 FF FF FF FF ...`, CIRAM,
  palette and OAM zero, registers zero, S = `$FD`.
- Controllers: FCEUX's model (strobe on the 1 -> 0 edge of `$4016` bit 0, 1s after 8 reads, open-bus bits
  6-7 from the last bus value).
- Mapper 7: FCEUX latches every write to `$4020-$FFFF`: PRG bank = bits 0-2, nametable page = bit 4. Real
  AOROM ANDs the value with the ROM byte (bus conflict); we count such writes instead (there are none).

Known differences from real hardware that we keep on purpose (they are what the TAS needs): vblank/NMI and
sprite-0 timing at scanline granularity, OAM DMA = 512 cycles (hardware 513/514), no PPU warm-up period,
`$2004` reads return the latch, no odd-frame rule tied to rendering.

## Sharing the CPU with generated code (for the M5 load-time recompiler)
`src/runtime/cpu.h`: native code keeps the registers in `Cpu`, accesses memory through `cpu_bus_read` /
`cpu_bus_write` (same side effects and open-bus value as the interpreter), charges time with
`cpu_add_cycles`, and runs any routine it does not have natively with `cpu_call(cpu, addr, limit)`: a JSR
into the interpreter that returns at the matching RTS (or reports a stack unwind, a jam or the instruction
limit). Checked by `cpu_call_test`. Open design point for M5: today the PPU frame loop drives the CPU in time
slices (`cpu_run`); native routines must also yield at the same points (or report their cycle cost so the
PPU can catch up) for lockstep to hold. CMake has a `BTDD_LIBTCC` option reserved (off, not wired yet).

## Not modelled yet (no effect on the 2P TAS)
- DMC DMA cycle stealing and DMC/frame-counter IRQs (`$4015` reads return a stub). The game never enables
  DMC and runs with I set.
- Unofficial opcodes: trapped (logged, CPU jams). nestest's unofficial section is therefore not run.
- fm2 reset/power commands mid-movie (neither TAS uses them).
- FCEUX's **new PPU** (dot-based). The 1P TAS (`tasvideos-2312S-1p.fm2`, `NewPPU 1`) needs it, so it cannot
  be checked yet. Options: implement a dot-accurate PPU as a second timing mode (also closer to hardware), or
  re-time the 1P movie.
- Sprite drawing (only evaluation and sprite-0 hit), color output, APU sound.

## How the FCEUX reference dumps are made
FCEUX's `-cfg` option does not work for this: it prefixes the path with FCEUX's own folder (an absolute
path is silently ignored) and loading a second config crashed it. So the comparison runs use a private copy
of the FCEUX folder, `..\Gamefiles\Movies\m1\fceux-oldppu\`, whose `fceux.cfg` has `newppu 0` (everything
else as in `..\tools\fceux\fceux.cfg`, which keeps `newppu 1` for the 1P movie). Run it from bash so the
environment reaches the Lua script:
```
cd /c/Users/theop/claude/Battle/Gamefiles/Movies/m1/fceux-oldppu
W='C:\Users\theop\claude\Battle'
BTDD_RAMDUMP="$W\\Gamefiles\\Movies\\m1\\fceux_2p.bin" ./fceux64.exe -readonly 1 \
  -playmovie "$W\\Gamefiles\\Movies\\tasvideos-4323M-2p.fm2" -lua "$W\\btdd-port\\tools\\fceux\\ramdump.lua" \
  "$W\\Gamefiles\\Battletoads-Double Dragon (USA).nes"
```
It opens a window, plays at maximum speed (about 1 minute for the whole movie) and closes itself. Maximum
speed is safe: in the Windows build frame skipping only skips drawing, emulation is unchanged.
`tools/fceux/watch.lua` and `exec.lua` log RAM writes / instruction starts with FCEUX's cycle counter; they
line up with `btdd_headless --watch` / `--exec` output for timing work.

## What M2 needs
- Sprite drawing into the line buffer (8 per line, priority, sprite-0/back-priority rules as FCEUX draws them),
  then NES palette -> RGB. `Ppu.fb` already holds the background as palette values (bit 6 = transparent).
- `src/platform`: SDL3 window (scaling, vsync / 60.0988 Hz pacing), keyboard and gamepad -> `nes_set_pads`.
  Swap `btdd_sdl_probe` for the real `btdd` front end.
- Keep `btdd_headless` + `ramdiff.py` as the regression gate: any change to `src/runtime` or `src/nes` must
  keep the 2P TAS identical.
- M3 can start from the APU write hook (`Nes.apu_hook`, CPU-cycle stamped).
