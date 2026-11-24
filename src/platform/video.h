/* Window, scaling and on-screen text for btdd_play (SDL3 renderer, no shaders).
 * Scaling filters:
 *   nearest  - square pixels at the largest integer scale (or 8:7 pixels in 4:3 mode, nearest)
 *   sharp    - "sharp bilinear": nearest to an integer multiple first, then bilinear to the final size, so
 *              pixels stay crisp and evenly sized at any window size or aspect
 *   bilinear - plain bilinear from 256x240
 * Scanlines: optional dark line under every NES row (applied at the integer-scaled stage, needs scale >= 2). */
#ifndef BTDD_VIDEO_H
#define BTDD_VIDEO_H
#include "gpu.h"
#include <SDL3/SDL.h>
#include <stdint.h>

enum { FILTER_NEAREST, FILTER_SHARP, FILTER_BILINEAR, FILTER_COUNT };
extern const char *const video_filter_names[FILTER_COUNT];

typedef struct Video {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;     /* 256x240 picture */
    SDL_Texture *big;     /* integer-scaled intermediate (sharp filter, scanlines) */
    int big_scale;
    int filter, aspect, scanlines;
    int pic_w;            /* picture width: 256, or 424 with the widescreen mod (SDL renderer path) */
    Gpu *gpu;             /* M10 GPU shader path (NULL = SDL renderer) */
    int preset;           /* GpuPreset when gpu is set */
    SDL_Surface *overlay; /* text for the GPU path, drawn by a software renderer */
    SDL_Renderer *overlay_ren;
} Video;

/* renderer: SDL render driver name or NULL/"auto"; use_gpu: try the GPU shader path first (preset = GpuPreset),
 * falling back to the SDL renderer. Returns 0 on success. */
int video_open(Video *v, const char *title, int scale, int aspect, int fullscreen, int filter, int scanlines,
               const char *renderer, int use_gpu, int preset);
/* Draws the picture (0xFFRRGGBB, 256x240) plus optional text lines at the top-left, then presents. */
void video_present(Video *v, const uint32_t *pixels, const char *const *text, int text_lines);
/* Picture width for the next frames (256 or a wide picture); -1 if this path cannot (GPU path for now). */
int video_set_width(Video *v, int w);
void video_close(Video *v);
int video_filter_from_name(const char *name); /* -1 if unknown */

#endif
