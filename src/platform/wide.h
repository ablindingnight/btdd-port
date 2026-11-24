/* M9 widescreen prototype (display only): a 424x240 picture = the game's 256x240 picture in the middle and 84
 * columns on each side drawn by the port from the level's block map (the NES has one 256-pixel nametable here, so
 * the extra columns exist only in the level data; docs/WIDESCREEN_NOTES.md, DATA_FORMATS.md sections 1-3).
 *
 * How the margins are placed: world X = camera X ($8E:$8D) + screen x; world Y = camera Y ($90:$8F) + screen y +
 * dy, where dy is found per frame by drawing the map under the visible picture and matching it against the
 * background the PPU drew (sample lines; last frame's dy first). Each line is then checked the same way: a line
 * whose middle does not match the map (status bar, transitions, text screens, the generated map of stage 4) gets
 * black margins, so nothing wrong is ever drawn. Blocks are drawn with the live CHR-RAM, palette and pattern
 * table; columns outside the map are black.
 *
 * Game logic is untouched: this reads RAM / PPU state after a frame. Sprites outside the 256-pixel window are not
 * drawn yet (the game culls them; see the notes). */
#ifndef BTDD_WIDE_H
#define BTDD_WIDE_H
#include "nes/nes.h"
#include "platform/screen.h"
#include <stdint.h>

#define WIDE_MARGIN 84
#define WIDE_W (PPU_W + 2 * WIDE_MARGIN) /* 424: about 16:9 with 8:7 pixels */

typedef struct WideState {
    int dy, dx, have_dy;      /* calibration, kept between frames */
    int retry_wait;           /* frames until the next full search after a miss */
    int sprites;              /* draw the entities the game culls (outside its window) into the margins */
    int hud;                  /* move the status bar halves to the edges */
    int hud_lines;            /* last frame: lines treated as the status bar */
    /* last frame's statistics */
    int lines_widened;        /* lines whose margins were drawn from the map */
    int margin_entities;      /* entities drawn into the margins */
    int calib_match;          /* per mille of matching sample pixels at the chosen dx, dy */
    int level, map_cols, map_rows;
} WideState;

/* Builds the wide picture (palette indices, emphasis per line in s->emph) from the frame just finished. */
void wide_render(WideState *ws, const Nes *nes, const Screen *s, uint8_t out[PPU_H][WIDE_W]);
/* 0xFFRRGGBB conversion like screen_to_rgb, for a WIDE_W-wide index picture */
void wide_debug(const Nes *nes); /* development: stderr dump of the map/nametable relation */
void wide_to_rgb(const Screen *s, const uint8_t idx[PPU_H][WIDE_W], uint32_t *out, int pitch, int fade);

#endif
