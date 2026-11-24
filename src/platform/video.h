/* Window, scaling and on-screen text for btdd_play (SDL3 renderer, no shaders).
 * Scaling filters:
 *   nearest  - square pixels at the largest integer scale (or 8:7 pixels in 4:3 mode, nearest)
 *   sharp    - "sharp bilinear": nearest to an integer multiple first, then bilinear to the final size, so
 *              pixels stay crisp and evenly sized at any window size or aspect
 *   bilinear - plain bilinear from 256x240
 * Scanlines: optional dark line under every NES row (applied at the integer-scaled stage, needs scale >= 2). */
#ifndef BTDD_VIDEO_H
#define BTDD_VIDEO_H
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
} Video;

/* renderer: SDL render driver name or NULL/"auto". Returns 0 on success. */
int video_open(Video *v, const char *title, int scale, int aspect, int fullscreen, int filter, int scanlines,
               const char *renderer);
/* Draws the picture (0xFFRRGGBB, 256x240) plus optional text lines at the top-left, then presents. */
void video_present(Video *v, const uint32_t *pixels, const char *const *text, int text_lines);
void video_close(Video *v);
int video_filter_from_name(const char *name); /* -1 if unknown */

#endif
