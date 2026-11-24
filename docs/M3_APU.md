# M3: APU (sound), 2026-11-23/24

## Result
- **Per-frame channel state identical to FCEUX 2.6.6 over the whole 2P TAS**: all 59,351 rows of
  `..\Gamefiles\Movies\musiclog\full\apu.csv` (pitch, volume and duty of both pulses, triangle pitch and gate,
  noise pitch/volume/mode, DMC state) match `btdd_apu_check --csv`, with MSVC, clang and gcc builds.
  Checked by `tools/apudiff.py`. The reference log varies a lot (about 800 different pitches per pulse
  channel, all 16 volume levels), so this is a real test of envelopes, sweeps, length/linear counters and
  the frame counter timing.
- **Same for the 1P TAS on the new-PPU model**: a fresh `musiclog.lua` capture from the stock FCEUX (newppu 1),
  `..\Gamefiles\Movies\m3\musiclog_1p\apu.csv`, is identical on all 83,576 rows
  (`btdd_apu_check ... --movie tasvideos-2312S-1p.fm2 --ppu new`), and that capture's ram.bin matches our RAM
  on every frame as well.
- With the APU attached (frame/DMC IRQ lines, DMC fetch stalls), the RAM is still identical to FCEUX on every
  frame of the 2P TAS; the `btdd_headless` RAM gates (2P old PPU, 1P new PPU) are unchanged.
- `apu_synth_test`: silence at power-on and with channels off, pulse at 440 Hz, triangle at 220 Hz, noise
  audible, `$4011` writes timed at 1 kHz reproduce a 1 kHz tone.
- Listening file (not in the repo): `..\Gamefiles\Movies\m3\btdd_2p_first120s.wav` (48 kHz mono, first two
  minutes of the 2P TAS: title, attract mode and the start of level 1). No clipping; peaks about 0.7.

## Design (`src/nes/apu.c`, `apu.h`)
Two layers:
1. **State that behaves like FCEUX**: envelopes, sweeps (including the "add" overflow mute and the
   one's-complement negate on pulse 1), length counters, triangle linear counter, frame counter (steps every
   7457.5 cycles, 4/5-step modes, IRQ in 4-step mode, `$4017` write resets it), DMC (period table, sample
   fetch with 4 stolen CPU cycles, loop/IRQ, 7-bit output counter), `$4015` read/write.
   `apu_tick(cycle)` advances it once per CPU instruction, at instruction start after the base cycles,
   which is where FCEUX does it; register writes go through `apu_write(cycle, addr, value)`.
2. **Synthesis**: each channel's timer and waveform is clocked per CPU cycle between events, so a `$4011` write
   (the game's PCM player at `$89AF`, ~11 per frame) lands on its exact cycle. Mixer: the standard nonlinear
   NES formulas (lookup tables). Downsampling: box average over each 48 kHz output period, then the console's
   output stage (high-pass 90 Hz and 440 Hz, low-pass 14 kHz). Output: float samples in [-1, 1].

Written from FCEUX's observable behaviour (its source in `..\tools\fceux-src\` was the specification) and the
NESdev documentation; no FCEUX code.

## Tools
- `btdd_apu_check ROM --movie X.fm2 [--csv out.csv] [--wav out.wav --wav-seconds 120] [--dump-ram out.bin]`:
  plays the movie on the interpreter with the APU attached. It drives the APU from the CPU's existing
  per-instruction hook (`Cpu.exec_hook` with every address watched) and `Nes.apu_hook`, so no runtime file
  had to change. CSV row 1 = power-on state, row f = after frame f-1, which is how `musiclog.lua` rows line up.
- `python tools/apudiff.py reference.csv candidate.csv`: per-row comparison; `*_key` pitch columns within
  0.011 (they are printed with 2 decimals), everything else exact.

## Not covered yet
- Neither TAS plays a DMC sample (the game only uses raw `$4011` writes), so DMC fetch/IRQ/loop are
  implemented but not checked against FCEUX.
- The waveform itself is only unit-tested, not compared sample by sample with FCEUX (FCEUX's Lua cannot
  record audio). FCEUX's own synthesis differs in details (its triangle/noise phase, its filters), so a
  sample-exact match is not the goal; the per-frame state is.

## What M2 needs to play the sound (integration into the runtime)
1. In `src/nes/nes.c`: own an `Apu`; call `apu_write` from the existing `apu_write` sink, return
   `apu_read_status` for `$4015` reads (today a stub), and pass the IRQ lines (`ApuHost.irq`) into
   `cpu.irq` (`CPU_IRQ_FRAME`, `CPU_IRQ_DMC`). DMC fetches: `ApuHost.read` = CPU bus read,
   `ApuHost.stall` = `cpu_add_cycles`.
2. In `src/runtime/cpu.c`: a dedicated per-instruction tick callback (instead of the debug `exec_hook`) at the
   same spot: after the base cycles are charged, before the instruction runs.
3. Each frame: `apu_render(apu, cpu.cycles)`, then push `apu_take()` samples (about 800 per frame) into an
   `SDL_AudioStream` (SDL3, 48 kHz mono float). Pace on audio: if the stream holds more than ~3 frames of
   audio, wait; if it runs dry, the emulator is late.
4. Both RAM gates and `apudiff` must stay identical after the integration.
