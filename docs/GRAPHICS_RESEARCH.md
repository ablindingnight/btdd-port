# Graphics for big screens: research and plan (M10, 2026-11-24)

What recent official retro collections and the leading emulator filters offer, what players like and dislike, which
algorithms we may ship (licences), and the option set we recommend. Then the first prototype (branch `m10-gfx`).
Sources are listed at the end; "unverified" means no primary source confirmed it. Web research done 2026-11-24.

## 1. Official collections

| Feature | Digital Eclipse (TMNT Cowabunga, Atari 50, Mega Man Legacy, Disney Classic, Karateka) | Limited Run Carbon Engine | Nintendo Switch Online NES | M2 Castlevania Anniversary / Advance | Capcom Arcade Stadium | Evercade |
|---|---|---|---|---|---|---|
| Integer / "pixel perfect" | worded as "Original" (sharpest at correct aspect) or "Sharp Screen" [M1][D1]; Atari 50 users fall back to custom size [A2] | "raw, unfiltered" mode [C1] | yes, "Pixel Perfect" (square pixels) [N1] | Anniversary: "pixel perfect" [CV1]; Advance: 3 aspect modes only [CA1] | ? | yes [E1] |
| 4:3 / corrected aspect | yes [T1][A1][M1][D1][K2] | yes [C1] | yes, "4:3" [N1] | yes | yes, but the bezel stays and the picture shrinks [CS1] | ? |
| Stretch / zoom / 16:9 | yes (full, stretch, zoom) [T1][A2][M1][D1] | ? | ? | 16:9 [CV1] | ? | ? |
| Separate smoothing | only as part of the TV filter [A1] | ? | only inside CRT mode; users ask for a toggle [N2] | ? | ? | bilinear (user report) [E2] |
| CRT / TV filter | yes: TV / Monitor / LCD; Karateka's Monitor filter praised [T1][A1][M1][D1][K1] | yes [C1] | yes; overhauled Nov 2025 (rounded corners, slight curve, warmer, crisper) [N2] | scanlines only (Anniversary) [CV1], none (Advance) [CA1] | yes, but lettered A-G without descriptions (Capcom Fighting Collection) [CS1] | ? |
| NTSC / composite simulation | not found | not found | not found | not found | not found | not found |
| Borders / wallpapers | toggle + wallpapers [T1][T3]; one static border (Disney) [D1] | ? | Switch Online frame; users want plain black [N2] | 3 borders [CV2] | cabinet bezels [CS1] | ? |
| Rewind / save states | rewind + saves (TMNT) [T3]; Disney rewind only a few minutes [D1] | rewind + saves shown as normal saves [C1][C2] | rewind (hold ZL+ZR) + suspend points [N3] | Advance: unverified | unverified | ? |
| Sprite-flicker removal / widescreen | not found / stretch only | game-aware hooks exist (River City Girls Zero) [C3] | not found | not found | n/a | not found |

Takeaways: the common menu is aspect (original / full / stretch), a TV-style filter, borders, rewind and saves. Real
integer scaling is labelled only by Nintendo and M2. **No official release surveyed offers NTSC/composite simulation,
sprite-flicker removal or widescreen**; those exist only in emulators (RetroArch, Mesen). We have the PPU's palette
indices and emphasis bits and our own flicker-free renderer (M7 `nolimit`), so offering them would beat every
official release surveyed.

## 2. What players praise and complain about

Praised:
- The Switch Online 2025 CRT filter is the benchmark among official releases (rounded edges, vignette, depth);
  TechRadar called the SNES version "shockingly brilliant" [N4]. The NES app got it in Nov 2025; Nintendo Life calls the
  gains subtle but welcome [N2].
- Filters that look like real hardware: Atari 50's TV filter compared against a RetroTINK [A1]; Karateka's Monitor
  filter and borders [K1]; even, square scanlines (Street Fighter 30th) [CS2].

Complained about:
- Scanlines that are just black lines over the picture, or CRT filters that are dark and blurry [N4][G1][G2].
- **Uneven scanlines / shimmer at non-integer scale** (Capcom Arcade Stadium at 1080p) [CS1][CS3]. The most common
  technical complaint.
- Bezels that cannot be turned off or shrink the picture [CS1]; users wanting a plain black border (OLED burn-in) [N2].
- Smoothing only bundled with the CRT mode [N2]; filters with no description [CS1]; too few options compared with
  emulators [CA1]; no scanline-free 4:3 at launch [CV4].
- Input lag (Mega Man X Legacy Collection, fixed later with a V-Sync toggle) [M2][M3]; rewind too short [D1].

## 3. Algorithms and shaders, with licences

"Ship?": **yes** = public domain or permissive (keep the notice), **no** = copyleft (GPL/LGPL would bind our code or force
dynamic linking), **reimpl** = only an own implementation from the published idea. Licences were read from the file
headers in libretro/slang-shaders and glsl-shaders; those repositories have **no repository-wide licence**, files
without a header must not be reused.

| Name | Author | Look | GPU cost | Licence | Ship? |
|---|---|---|---|---|---|
| Nearest, integer scale | - | perfectly sharp square pixels | trivial | - | yes |
| sharp-bilinear (+ -simple) | Themaister; rsn8887 | crisp pixels, no shimmer at any size | trivial | public domain | yes |
| bandlimit-pixel, aann | Themaister; (aann header) | anti-aliased pixel edges | trivial | MIT | yes |
| xBR, Super-xBR | Hyllian | edge-directed smoothing, rounded diagonals | low / medium | MIT | yes |
| ScaleFX | Sp00kyFox | cleanest rounded "vector" look; can over-round | high (5 passes) | MIT | yes |
| OmniScale | Lior Halphon | any-factor edge smoothing | medium | MIT | yes |
| MMPX | Morgan McGuire, Mara Gagiu (JCGT 2021) | style-preserving 2x: keeps lines and shapes, no blur | low | MIT | yes |
| xBRZ (+ libretro xbrz shaders) | Zenju (+ Hyllian) | crisp accurate edges | medium | GPL-3.0 (shaders reproduce the GPL notice) | no |
| HQx | Maxim Stepin; C lib grom358 | smooth, slightly blobby | medium (LUT) | LGPL-2.1+ (original: unverified) | no |
| 2xSaI, SuperEagle | Derek Liauw; DOSBox | old soft smoothing | low | GPL (version unstated) | no (EPX/Eagle *ideas* are trivial: reimpl) |
| CRT-Lottes | Timothy Lottes | sharp mask CRT, soft scanlines, bloom | low | public domain | yes |
| crt-geom, zfast-crt, crt-guest-advanced, CRT-Royale (+ ReShade port) | cgwg et al.; Greg Hogan; guest(r); TroggleMonkey | curved CRT; fast CRT; top tunable CRT; most physical CRT | low ... very high | GPL-2.0+ | no |
| crt-easymode | EasyMode | flat CRT, good at 1080p+ | low | GPL (version unstated) | no |
| Maister NTSC shader | Themaister | composite encode/decode | medium | no header (unverified) | reimpl or ask the author |
| blargg nes_ntsc | Shay Green | reference NES composite (palette index + emphasis in) | CPU, cheap | LGPL | no (static); reimpl from the signal maths |
| NTSC-CRT | LMP88959 (EMMIR) | integer NTSC encode/decode with an NES mode | CPU, moderate | custom permissive (attribution optional) | yes |
| NES-CVBS | Gumball2415 | NES composite encode/decode (WIP) | CPU | MIT | yes |
| ReShade framework | crosire | injector | - | BSD-3 (each shader separately) | n/a |

**Permissive set we can port**: sharp-bilinear (PD), CRT-Lottes (PD), bandlimit-pixel / aann (MIT), xBR / Super-xBR
(MIT), ScaleFX (MIT), OmniScale (MIT), MMPX (MIT), NTSC-CRT (permissive), NES-CVBS (MIT). MIT ones need their notice in
a THIRD_PARTY file. Avoid xBRZ, HQx, 2xSaI and every GPL CRT shader (guest, Royale, geom, zfast, easymode).

## 4. Shaders with SDL3's GPU API
- Each backend needs its own bytecode: DXBC/DXIL (Direct3D 12), SPIR-V (Vulkan), MSL/metallib (Metal); resource
  bindings follow SDL's fixed layout (HLSL: fragment textures/samplers in space2, uniforms in space3). libretro
  `.slang` files cannot be dropped in; every shader is ported to that layout anyway.
- SDL_shadercross (zlib; uses SPIRV-Cross, Apache-2.0, and DXC) converts HLSL to all formats. Recommended: compile
  offline at build time, embed the blobs, ship no compiler.

## 5. Recommended option set

Geometry: NES pixels are 8:7, so the full picture is about 292x240 (4:3 when 8 lines are cropped top and bottom).
Vertical integer scales fit 1440p (6x) and 4K (9x) exactly; 1080p is 4x with bars or 4.5x. Horizontal 8:7 is never
integer, so **integer (or sharp) vertical + sharp-bilinear horizontal** gives even pixels and even scanlines.

- **Default "Sharp"**: 8:7 aspect, sharp bilinear (integer vertical where it fills the screen within ~15 %), black
  border, no scanlines. Matches Switch Online "4:3" and Digital Eclipse "Original" and looks right at any size.
- **Presets**:
  1. *Pixel Perfect*: square pixels, strict integer scale, nearest. For purists and capture.
  2. *CRT*: our own CRT (CRT-Lottes ideas are public domain) with beam profile, aperture-grille / shadow mask,
     brightness compensation (against "too dark"), optional curvature + rounded corners (the Switch Online 2025 look);
     simplified mask under 4x.
  3. *CRT + Composite*: an NES composite decode straight from palette indices and emphasis (NTSC-CRT, NES-CVBS or our
     own), then the CRT. Not offered by any official release surveyed.
  4. *Smooth*: MMPX 2x (MIT) or ScaleFX (MIT), then sharp bilinear, for an "HD" look; xBR as a cheaper option.
- **Separate toggles** (not bundled): aspect (8:7 / 4:3 / 1:1 / stretch), overscan crop (0 / 8 px), scanline
  strength, mask strength, brightness, border (black or a simple wallpaper; never forced bezels), sprite-flicker fix
  (M7 `nolimit`), rewind and save states (M8). Every filter gets a one-line description and a live preview in the
  menu (unlike lettered filter lists).
- Low latency: V-Sync on, no extra frames of buffering; the shader chain stays within one frame.

## 6. Prototype (branch `m10-gfx`)

- `src/platform/gpu.c`: SDL3 GPU device (Direct3D 12), chain *upscale pass (optional) -> output pass -> text overlay*,
  cached pipelines, offscreen rendering for screenshots. Shaders in `shaders/*.hlsl` (all our own code), compiled by
  `tools/build_shaders.py` (fxc, DXBC SM 5.1) into the committed `src/platform/gpu_shaders.h`.
- Presets: `nearest`, `sharp` (sharp bilinear), `bilinear`, `crt` (gaussian beams whose width grows with brightness,
  aperture-grille mask, linear-light gamma, brightness compensation), `smooth` (2x upscaler from the EPX rule, then
  sharp bilinear), `smooth-crt` (the upscaler, then the CRT with its beams kept at 240 lines).
- btdd_play: `video = gpu` and `shader = <preset>` in btdd.cfg; F3 cycles the presets. If the GPU device cannot start
  (no D3D12, or `SDL_GPU_DRIVER=vulkan`), it falls back to the SDL renderer (checked). Emulation is untouched.
- `btdd_gfxshot ROM --movie X --frames N --size WxH --out DIR` renders the presets offscreen.
- Screenshots (game pictures, so outside the repo) in `..\Gamefiles\Movies\m10\`: frames 2000, 7300, 16500, 30000 of
  the 2P TAS at 1440x1080 for all six presets (`f<frame>_<preset>_1440x1080.png`), frame 16500 at 1280x960 (all) and
  2880x2160 (sharp, crt, smooth-crt).

Next steps: SPIR-V and MSL blobs (SDL_shadercross offline) so Vulkan/Metal get the shaders too; an MMPX port (MIT; done, section 7); the CRT + Composite preset (composite from palette indices); integer-vertical geometry
as the default; overscan crop and curvature options; tuning the CRT on real 1440p/4K screens.

## 7. Pixel-art upscalers (branch `m10-upscale`, 2026-11-25)

Two more GPU presets, both permissively licensed (notices in `third_party/README.md`), selectable with F3, in the
in-game Graphics page and in the launcher's Graphics page (`shader = mmpx | xbr` in btdd.cfg):

| Preset | Shader | How it scales | Licence |
| --- | --- | --- | --- |
| `mmpx` | `shaders/mmpx.frag.hlsl`: MMPX 2x (McGuire & Gagiu, JCGT 2021); keeps 1:1 and 2:1 lines, intersections and triangle tips, never invents colours | 2x pass to an offscreen texture, then sharp bilinear to the window: exact at 4x, crisp at 2x, 3x or any size | MIT (port of the reference rules) |
| `xbr` | `shaders/xbr.frag.hlsl`: xBR edge rule (Hyllian) with its interpolation restriction, evaluated per output pixel | one pass at the output size: any scale (2x, 3x, 4x, fullscreen), one output pixel of antialiasing on edges | own code from the MIT-published rule |

Both work with the M9 widescreen picture (the passes follow the frame width). Display only: the emulation gates are
unchanged. Not chosen: xBRZ (GPL), ScaleFX (MIT, but five passes; possible later). Screenshots (private) in
`..\Gamefiles\Movies\m10\upscale\`: frame 3000 of the 2P TAS at 512x480, 768x720 and 1024x960 (nearest, mmpx, xbr)
and 1696x960 widescreen (mmpx, xbr).

## Sources
- Digital Eclipse: [T1] https://videogameschronicle.com/review/tmnt-the-cowabunga-collection ; [T3] https://www.nintendolife.com/reviews/nintendo-switch/teenage-mutant-ninja-turtles-the-cowabunga-collection ; [A1] https://forums.atariage.com/topic/337463-atari-50-the-anniversary-celebration-compilation-will-include-jaguar-games/page/11/ ; [A2] https://forums.atariage.com/topic/363979-2600-scanlines-option/ ; [M1] https://www.gameinformer.com/games/mega_man_legacy_collection/b/xboxone/archive/2015/07/15/capcom-releases-new-details-about-mega-man-legacy-collection-screen-options-challenge-mode-and-more.aspx ; [M2] https://steamcommunity.com/app/363440/discussions/0/4139438126592159745 ; [M3] https://themmnetwork.com/2018/07/31/whats-the-hold-up-input-lag-tests-for-mega-man-x-legacy-collection/ ; [D1] https://wccftech.com/review/disney-classic-games-aladdin-and-the-lion-king/ ; [K1] https://seafoamgaming.com/2023/09/11/the-making-of-karateka-steam-review/ ; [K2] https://gamingnexus.com/Article/12921/The-Making-of-Karateka
- Carbon Engine: [C1] https://press-start.com.au/reviews/ps5-reviews/2024/08/02/tombi-special-edition-review-pixel-perfect-pig-punting/ ; [C2] https://www.nintendolife.com/features/how-limited-runs-new-engine-respects-the-legacy-of-classic-games ; [C3] https://limitedrungames.com/blogs/news/bringing-new-life-to-games-with-carbon
- Switch Online: [N1] https://www.nintendo.co.uk/Support/Nintendo-Switch/Nintendo-Switch-Online/How-to-Change-the-Game-Display-Mode-1441499.html ; [N2] https://www.nintendolife.com/news/2025/11/psa-nes-classics-have-had-a-crt-filter-overhaul-in-latest-switch-online-update ; [N3] https://www.videogameschronicle.com/news/nintendo-confirms-nes-rewind-feature-for-switch-online/ ; [N4] https://www.techradar.com/gaming/nintendo/im-stunned-nintendo-switch-onlines-updated-snes-crt-filter-is-actually-brilliant
- M2 / Capcom / Evercade: [CV1] https://www.nintendolife.com/reviews/switch-eshop/castlevania_anniversary_collection ; [CV2] https://www.nintendoworldreport.com/review/50554 ; [CV4] https://steamcommunity.com/app/1018010/discussions/0/3264459260603799124 ; [CA1] https://kotaku.com/Castlevania-advance-collection-is-great-but-emulators-1847798120 ; [CS1] https://www.pcgamer.com/capcoms-treasure-trove-of-classic-fighting-games-deserves-much-better-graphics-options , https://steamcommunity.com/app/1685750/discussions/0/3415433928537804998 ; [CS2] https://steamcommunity.com/app/586200/discussions/0/1696048879935153823/?ctp=1 ; [CS3] https://www.nintendolife.com/reviews/switch-eshop/capcom-arcade-2nd-stadium ; [E1] https://evercade.co.uk/support-vs/vs-patch-notes/ ; [E2] https://forums.atariage.com/topic/363979-2600-scanlines-option/
- General: [G1] https://www.gamesradar.com/hardware/retro/what-are-scanlines/ ; [G2] https://www.pressreader.com/uk/retro-gamer/20200806/284326842064696
- Licences: file headers in https://github.com/libretro/slang-shaders and https://github.com/libretro/glsl-shaders ; xBRZ https://sourceforge.net/projects/xbrz/ ; HQx https://github.com/grom358/hqx ; MMPX https://casual-effects.com/research/McGuire2021PixelArt/ ; blargg https://slack.net/~ant/libs/ntsc.html ; NTSC-CRT https://github.com/LMP88959/NTSC-CRT ; NES-CVBS https://github.com/Gumball2415/NES-CVBS ; ReShade https://github.com/crosire/reshade
- SDL: https://wiki.libsdl.org/SDL3/SDL_CreateGPUShader ; https://github.com/libsdl-org/SDL_shadercross
