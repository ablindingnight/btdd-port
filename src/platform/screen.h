/* Builds the displayed 256x240 picture from the PPU, one scanline at a time, through the PPU's line_done
 * hook (display only: it never changes emulation state).
 *  - Old (FCEUX old-PPU) model: the PPU has drawn the line's background lazily; the sprites for the line
 *    are in Ppu.line_sprites. They are composited here the way FCEUX does it.
 *  - New (dot) model: the PPU already produced final pixels; they are copied.
 * Output: NES palette indices (0-63) plus the emphasis bits per line; screen_to_rgb converts with a
 * generated NTSC palette. Platform independent (no SDL). */
#ifndef BTDD_SCREEN_H
#define BTDD_SCREEN_H
#include "nes/ppu.h"
#include <stdint.h>

typedef struct Screen {
    uint8_t idx[PPU_H][PPU_W]; /* palette index per pixel */
    uint8_t emph[PPU_H];        /* PPUMASK bits 5-7 per line */
    /* "nolimit" statistics: lines that had more than 8 sprites, and (old model) lines where our own sprite
     * evaluation disagreed with the PPU's first 8 (then the PPU's are shown; should stay 0) */
    uint32_t extra_sprite_lines, sprite_mismatch_lines;
    uint8_t line_extra[PPU_H]; /* 1 if the line was drawn with more than 8 sprites */
} Screen;

void screen_attach(Screen *s, Ppu *ppu);      /* call after nes_power (power clears the hook) */
void screen_palette(uint32_t rgb[8][64]);     /* 0xRRGGBB for every emphasis combination and color */
/* Replaces the generated palette: 64 colors (emphasis derived by dimming the other channels) or 512
 * (8 emphasis variants x 64, the usual .pal layouts). rgb holds count*3 bytes. Returns 0 if accepted. */
int screen_set_palette(const uint8_t *rgb, int count);
void screen_to_rgb(const Screen *s, uint32_t *out_argb, int pitch_pixels); /* 0xFFRRGGBB */
/* Same conversion for any index picture (w x h, idx_pitch bytes per row), emphasis per line. */
void screen_index_to_rgb(const uint8_t *idx, int idx_pitch, const uint8_t *emph, int w, int h, uint32_t *out, int pitch);
/* M7 mod "nolimit": draw every sprite on every line instead of the PPU's 8 (display only; global setting). */
void screen_set_no_sprite_limit(int on);

#endif
