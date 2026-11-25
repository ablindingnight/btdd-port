/* Drawing kit for the launcher: filled shapes and TTF text through SDL_RenderGeometry, with an affine transform
 * stack so any group (a tilted sticker, the logo) can be rotated. All coordinates are in the launcher's
 * 1280x720 design space; the renderer's logical presentation scales them to the window. */
#ifndef BTDD_LAUNCHER_KIT_H
#define BTDD_LAUNCHER_KIT_H
#include <SDL3/SDL.h>
#include <stdint.h>

#define KIT_W 1280
#define KIT_H 720

/* colours are 0xRRGGBB; alpha is separate (0..1) */
typedef uint32_t Rgb;

typedef enum KitFont { FONT_DISPLAY, FONT_LABEL, FONT_LABEL_IT, FONT_BODY, FONT_BODY_BOLD, FONT_MONO, FONT_COUNT } KitFont;
typedef enum KitAlign { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT } KitAlign;

int kit_init(SDL_Renderer *r, const char *font_dir); /* 0 on success; text is skipped if fonts are missing */
void kit_quit(void);
/* output pixels per design pixel: text is rasterized at this resolution so it stays sharp when scaled up */
void kit_set_pixel_scale(float s);
SDL_Renderer *kit_renderer(void);
const char *kit_error(void);

/* transform stack */
void kit_push(void);
void kit_pop(void);
void kit_translate(float x, float y);
void kit_rotate_at(float deg, float cx, float cy); /* rotate following drawing about (cx, cy) */
void kit_alpha(float a);                            /* multiplies every colour's alpha until the next pop */

void kit_rect(float x, float y, float w, float h, Rgb c, float a);
void kit_frame(float x, float y, float w, float h, float t, Rgb c, float a); /* outline of thickness t inside */
void kit_quad(const float xy[8], Rgb c, float a);
void kit_poly(const float *xy, int n, Rgb c, float a);  /* star-shaped about its centroid (fan) */
void kit_circle(float cx, float cy, float r, Rgb c, float a);
void kit_ring(float cx, float cy, float r_in, float r_out, Rgb c, float a);
void kit_ellipse_ring(float cx, float cy, float rx, float ry, float t, float deg, Rgb c, float a);
void kit_rrect(float x, float y, float w, float h, float rad, Rgb c, float a); /* rounded rectangle */
void kit_line(float x0, float y0, float x1, float y1, float t, Rgb c, float a);
void kit_gradient_v(float x, float y, float w, float h, Rgb top, Rgb bottom, float a);
void kit_texture(SDL_Texture *t, float x, float y, float w, float h, float a);

/* text: size in pixels (em); y is the top of the line box (ascent below it) */
float kit_text(KitFont f, float size, float x, float y, const char *s, Rgb c, float a, KitAlign al);
float kit_text_width(KitFont f, float size, const char *s);
float kit_text_ascent(KitFont f, float size);
/* comic text: ink stroke of `stroke` px, then an offset shadow (dx, dy) in `shadow`, then the fill */
float kit_text_ink(KitFont f, float size, float x, float y, const char *s, Rgb fill, Rgb ink, float stroke,
                   float sdx, float sdy, KitAlign al);
/* text wrapped to width w; returns the height used */
float kit_text_wrap(KitFont f, float size, float x, float y, float w, float line_h, const char *s, Rgb c, float a);

#endif
