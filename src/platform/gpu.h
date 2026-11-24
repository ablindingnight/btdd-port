/* GPU shader path for btdd_play (M10 prototype): SDL3's GPU API runs a small shader chain on the 256x240 picture.
 *
 *   NES picture (256x240) -> [upscale pass to an offscreen texture, optional] -> output pass to the window
 *   viewport (nearest / sharp bilinear / bilinear / CRT) -> overlay text on top.
 *
 * Shaders are our own HLSL in shaders/, compiled to DXBC by tools/build_shaders.py (D3D12 backend). Where the
 * GPU path cannot start (no D3D12, other platforms for now) video.c keeps the SDL_Renderer path. Display only:
 * emulation never sees any of this. */
#ifndef BTDD_GPU_H
#define BTDD_GPU_H
#include <SDL3/SDL.h>
#include <stdint.h>

typedef enum GpuPreset {
    GPU_PRESET_NEAREST,  /* square pixels, crisp (integer sizes look best) */
    GPU_PRESET_SHARP,    /* sharp bilinear: crisp and even at any size / 4:3 */
    GPU_PRESET_BILINEAR, /* smooth */
    GPU_PRESET_CRT,      /* scanline beams, aperture-grille mask, gamma */
    GPU_PRESET_SMOOTH,   /* pixel-art upscaler 2x (edges smoothed, colors kept), then sharp bilinear */
    GPU_PRESET_SMOOTH_CRT, /* the upscaler, then the CRT */
    GPU_PRESET_COUNT
} GpuPreset;
extern const char *const gpu_preset_names[GPU_PRESET_COUNT];
int gpu_preset_from_name(const char *name); /* -1 if unknown */

typedef struct Gpu Gpu;

/* Creates the device (D3D12 / DXBC) and, if win is not NULL, takes over the window's presentation.
 * Returns NULL (and logs why) if not possible. */
Gpu *gpu_open(SDL_Window *win);
void gpu_close(Gpu *g);
const char *gpu_driver(const Gpu *g);

/* One frame to the window: picture 0xFFRRGGBB 256x240, drawn into dst (window pixels); overlay (may be NULL) is an
 * RGBA surface drawn over dst with alpha. */
int gpu_present(Gpu *g, const uint32_t *pixels, GpuPreset preset, SDL_FRect dst, SDL_Surface *overlay);
/* The same chain into an offscreen w x h image (whole image = the picture), written as 0xFFRRGGBB to out. */
int gpu_render_offscreen(Gpu *g, const uint32_t *pixels, GpuPreset preset, int w, int h, uint32_t *out);

#endif
