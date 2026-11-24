/* M9 widescreen prototype (see wide.h). Reads the player's ROM through Nes.rom at run time (bank 5 tables); no game
 * data is stored here. */
#include "wide.h"
#include <string.h>

/* bank 5 level tables (DATA_FORMATS.md section 1): block set lo/hi, attributes lo/hi, map lo/hi, by level */
#define T_BLOCKS_LO 0xA4A1
#define T_BLOCKS_HI 0xA4AE
#define T_ATTR_LO 0xA4BB
#define T_ATTR_HI 0xA4C8
#define T_MAP_LO 0xA4D5
#define T_MAP_HI 0xA4E2

typedef struct Level {
    int ok;
    uint16_t map, blocks, attr; /* CPU addresses (bank 5) or RAM addresses (< $0800) */
    int cols, rows;
} Level;

static uint8_t byte_at(const Nes *n, uint16_t addr)
{
    if (addr < 0x0800)
        return n->ram[addr];
    if (addr >= 0x8000)
        return n->rom->prg[5][addr - 0x8000];
    return 0;
}

static uint8_t b5(const Nes *n, uint16_t addr)
{
    return n->rom->prg[5][addr - 0x8000];
}

static Level level_of(const Nes *n)
{
    Level L;
    int idx = n->ram[0x10];
    memset(&L, 0, sizeof L);
    if (n->ram[0xEA] && !(n->ram[0xEA] & 0x80))
        idx = n->ram[0xEA] + 9; /* stage-5 sub-areas (as bank 5 sub_a305) */
    if (idx < 0 || idx > 12)
        return L;
    L.map = (uint16_t)(b5(n, (uint16_t)(T_MAP_LO + idx)) | b5(n, (uint16_t)(T_MAP_HI + idx)) << 8);
    L.blocks = (uint16_t)(n->ram[0xBC] | n->ram[0xBD] << 8); /* live pointers set by bank 5 sub_a452 */
    L.attr = (uint16_t)(n->ram[0xBA] | n->ram[0xBB] << 8);
    L.cols = n->ram[0xBE];
    if (!L.cols || L.blocks < 0x8000 || L.attr < 0x8000)
        return L;
    if (L.map < 0x0800) /* level 4: map in RAM $0600-$067F */
        L.rows = (0x0680 - L.map) / L.cols;
    else if (L.map >= 0x8000 && L.blocks > L.map) /* the map is followed by its block set */
        L.rows = (L.blocks - L.map) / L.cols;
    L.ok = L.rows > 0;
    return L;
}

/* Background palette index of world pixel (wx, wy), or -1 outside the map; *opaque = pattern pixel not 0. */
static int map_pixel_o(const Nes *n, const Level *L, int wx, int wy, int *opaque)
{
    int bc = wx >> 5, br = wy >> 5, tile, row, bit, pix, q, attr;
    uint8_t block;
    unsigned addr;
    const Ppu *p = &n->ppu;
    if (wx < 0 || wy < 0 || bc >= L->cols || br >= L->rows)
        return -1;
    block = byte_at(n, (uint16_t)(L->map + br * L->cols + bc));
    tile = byte_at(n, (uint16_t)(L->blocks + block * 16 + ((wy >> 3) & 3) * 4 + ((wx >> 3) & 3)));
    row = wy & 7;
    bit = 7 - (wx & 7);
    addr = (((unsigned)p->ctrl & 0x10u) << 8) + (unsigned)tile * 16 + (unsigned)row;
    pix = ((p->chr[addr & 0x1FFF] >> bit) & 1) | (((p->chr[(addr + 8) & 0x1FFF] >> bit) & 1) << 1);
    *opaque = pix != 0;
    if (!pix)
        return p->pal[0] & 0x3F;
    attr = byte_at(n, (uint16_t)(L->attr + block));
    q = ((wy >> 4) & 1) * 2 + ((wx >> 4) & 1);
    return p->pal[((attr >> (q * 2)) & 3) << 2 | pix] & 0x3F;
}

static int map_pixel(const Nes *n, const Level *L, int wx, int wy)
{
    int o;
    return map_pixel_o(n, L, wx, wy, &o);
}

/* The background the PPU drew at (x, y), palette index (old model: sprites are not in fb; new model: fb is the
 * final picture, sprites included, so a few mismatches are expected there). */
static int ppu_bg(const Nes *n, int x, int y)
{
    return n->ppu.fb[y * PPU_W + x] & 0x3F;
}

/* Per mille of pixels in [16, 240) of line y that match the map with offsets (camX, wy0). */
static int line_match(const Nes *n, const Level *L, int camx, int wy, int y, int step)
{
    int x, hit = 0, all = 0;
    for (x = 16; x < 240; x += step) {
        int m = map_pixel(n, L, camx + x, wy);
        all++;
        if (m >= 0 && m == ppu_bg(n, x, y))
            hit++;
    }
    return all ? hit * 1000 / all : 0;
}

static const int sample_lines[] = {40, 72, 104, 136, 168, 200, 224};
#define SAMPLES (int)(sizeof sample_lines / sizeof sample_lines[0])

static int frame_match(const Nes *n, const Level *L, int camx, int camy, int dy)
{
    int i, sum = 0;
    for (i = 0; i < SAMPLES; i++)
        sum += line_match(n, L, camx, camy + sample_lines[i] + dy, sample_lines[i], 4);
    return sum / SAMPLES;
}

static int draw_margin_sprites(const Nes *n, uint8_t out[PPU_H][WIDE_W], const uint8_t *widened,
                               const uint8_t *bg_opaque);

void wide_render(WideState *ws, const Nes *n, const Screen *s, uint8_t out[PPU_H][WIDE_W])
{
    static uint8_t widened[PPU_H], bg_opaque[PPU_H * WIDE_W];
    Level L = level_of(n);
    int camx = n->ram[0x8D] | n->ram[0x8E] << 8;
    int camy = (int16_t)(n->ram[0x8F] | n->ram[0x90] << 8);
    int y, x, best = -1, best_dy = 0, best_dx = 0, dy, hud_end;

    ws->lines_widened = 0;
    ws->margin_entities = 0;
    memset(widened, 0, sizeof widened);
    memset(bg_opaque, 0, sizeof bg_opaque);
    ws->level = n->ram[0x10];
    ws->map_cols = L.cols;
    ws->map_rows = L.rows;
    for (y = 0; y < PPU_H; y++) { /* middle: the game's own picture, unchanged */
        memset(out[y], 0x0F, WIDE_MARGIN);
        memcpy(out[y] + WIDE_MARGIN, s->idx[y], PPU_W);
        memset(out[y] + WIDE_MARGIN + PPU_W, 0x0F, WIDE_MARGIN);
    }
    ws->calib_match = 0;
    if (!L.ok || !(n->ppu.mask & 0x08))
        return;

    /* calibrate the vertical offset: last frame's first, then a search */
    if (ws->have_dy) {
        best = frame_match(n, &L, camx + ws->dx, camy, ws->dy);
        best_dy = ws->dy;
        best_dx = ws->dx;
    }
    if (best < 950 && (!ws->retry_wait || --ws->retry_wait == 0)) {
        int dx;
        /* dx 0 first (the usual case), then small horizontal offsets */
        for (dx = 0; dx <= 8 && best < 950; dx = dx > 0 ? -dx : -dx + 1) { /* real offsets: -4..4 */
            for (dy = -288; dy <= 288; dy++) {
                int m = frame_match(n, &L, camx + dx, camy, dy);
                if (m > best) {
                    best = m;
                    best_dy = dy;
                    best_dx = dx;
                }
            }
        }
        if (best < 900)
            ws->retry_wait = 30; /* not the map (text screen, ...): search again in 30 frames */
    }
    ws->calib_match = best;
    if (best < 900) /* the picture is not the map (text screen, transition, generated map) */
        return;
    ws->dy = best_dy;
    ws->dx = best_dx;
    ws->have_dy = 1;
    camx += best_dx;

    /* status bar band: from the top down to the last line (above 48) that is not the map; its black gaps can match
     * a black sky, so the whole band counts as status bar */
    hud_end = 0;
    for (y = 0; y < 48; y++)
        if (line_match(n, &L, camx, camy + y + best_dy, y, 2) < 900)
            hud_end = y + 1;
    ws->hud_lines = hud_end;
    for (y = hud_end; y < PPU_H; y++) {
        int wy = camy + y + best_dy;
        if (line_match(n, &L, camx, wy, y, 2) < 900) /* a split or a text line: leave black */
            continue;
        ws->lines_widened++;
        widened[y] = 1;
        for (x = -WIDE_MARGIN; x < 0; x++) {
            int o = 0, m = map_pixel_o(n, &L, camx + x, wy, &o);
            out[y][WIDE_MARGIN + x] = (uint8_t)(m < 0 ? 0x0F : m);
            bg_opaque[y * WIDE_W + WIDE_MARGIN + x] = (uint8_t)(m >= 0 && o);
        }
        if (!(n->ppu.mask & 0x02)) /* the game hides its leftmost 8 pixels: fill them too, no seam */
            for (x = 0; x < 8; x++) {
                int o = 0, m = map_pixel_o(n, &L, camx + x, wy, &o);
                if (m >= 0)
                    out[y][WIDE_MARGIN + x] = (uint8_t)m;
                bg_opaque[y * WIDE_W + WIDE_MARGIN + x] = (uint8_t)(m >= 0 && o);
            }
        for (x = PPU_W; x < PPU_W + WIDE_MARGIN; x++) {
            int o = 0, m = map_pixel_o(n, &L, camx + x, wy, &o);
            out[y][WIDE_MARGIN + x] = (uint8_t)(m < 0 ? 0x0F : m);
            bg_opaque[y * WIDE_W + WIDE_MARGIN + x] = (uint8_t)(m >= 0 && o);
        }
    }
    if (ws->sprites)
        ws->margin_entities = draw_margin_sprites(n, out, widened, bg_opaque);
    if (ws->hud && ws->lines_widened) { /* status bar lines at the top: halves out to the edges */
        for (y = 0; y < hud_end; y++) {
            uint8_t line[PPU_W], fill = s->idx[y][0];
            memcpy(line, s->idx[y], PPU_W);
            memset(out[y], fill, WIDE_W);
            memcpy(out[y], line, PPU_W / 2);
            memcpy(out[y] + WIDE_W - PPU_W / 2, line + PPU_W / 2, PPU_W / 2);
        }
    }
}

/* ---- sprites in the margins ----
 * The game culls entities outside its 256-pixel window (bank 0 sub_8688 sets bit 7 of $04A3), so for those the port
 * runs the main entity drawer's frame-record decode itself (DATA_FORMATS.md section 7; bank 6 sub_8c15) and draws
 * the sprites into the margins only (inside the window the game's own sprites are already in the picture). The two
 * "special" entity types and the second group of two player frames are not handled (prototype). */
#define B6(n, a) ((n)->rom->prg[6][(uint16_t)(a) - 0x8000])
#define REC_LO_PLAYER 0xBFE1
#define REC_HI_PLAYER 0xC192
#define REC_LO_ENEMY 0xC093
#define REC_HI_ENEMY 0xC244
#define CHAR_PALETTE 0x8EBF

static void draw_entity(const Nes *n, int slot, int sx, int sy, uint8_t out[PPU_H][WIDE_W],
                        const uint8_t *widened, const uint8_t *bg_opaque)
{
    int id = n->ram[0x03D1 + slot], flags = n->ram[0x04A3 + slot] & 0x63, flip = flags & 0x40, k, count, seq;
    uint16_t rec;
    uint8_t base, hdr, tile = 0;
    int ax, ay;
    const Ppu *p = &n->ppu;
    if (!id)
        return;
    rec = slot < 2 ? (uint16_t)(B6(n, REC_LO_PLAYER + id) | B6(n, REC_HI_PLAYER + id) << 8)
                   : (uint16_t)(B6(n, REC_LO_ENEMY + id) | B6(n, REC_HI_ENEMY + id) << 8);
    if (rec < 0x8000)
        return;
    ax = flip ? sx + B6(n, rec) - 7 : sx - B6(n, rec);
    ay = sy + (int8_t)B6(n, rec + 1);
    base = B6(n, rec + 2);
    hdr = B6(n, rec + 3);
    rec += 4;
    seq = hdr & 0x80;
    count = hdr & 0x7F;
    if (seq)
        tile = B6(n, rec++);
    for (k = 0; k < count; k++) {
        int t, x, y, row, col, pal_sel;
        uint8_t attr;
        if (seq)
            t = tile + k;
        else
            t = B6(n, rec++);
        x = (int8_t)B6(n, rec++);
        y = (int8_t)B6(n, rec++);
        attr = (uint8_t)(base & 0x80 ? B6(n, rec++) : base);
        attr ^= (uint8_t)flags;
        x = flip ? ax - x : ax + x;
        y = ay - y + 1; /* OAM Y is one line above the picture */
        pal_sel = attr & 3;
        if (seq && tile <= 2) { /* player marker: per-character palette, player 2's own tile block */
            pal_sel = B6(n, CHAR_PALETTE + (n->ram[0x05F0 + slot] & 7)) & 3;
            if (slot == 1)
                t += 0x10;
        }
        for (row = 0; row < 8; row++) {
            int yy = y + row, prow = (attr & 0x80) ? 7 - row : row;
            unsigned a = (((unsigned)p->ctrl & 0x08u) << 9) + (unsigned)(t & 0xFF) * 16 + (unsigned)prow;
            if (yy < 0 || yy >= PPU_H || !widened[yy])
                continue;
            for (col = 0; col < 8; col++) {
                int xx = x + col, bit = (attr & 0x40) ? col : 7 - col, pix, ox;
                pix = ((p->chr[a & 0x1FFF] >> bit) & 1) | (((p->chr[(a + 8) & 0x1FFF] >> bit) & 1) << 1);
                if (!pix)
                    continue;
                ox = xx + WIDE_MARGIN;
                if (ox < 0 || ox >= WIDE_W || (xx >= 8 && xx < PPU_W)) /* margins (and the hidden left 8) only */
                    continue;
                if ((attr & 0x20) && bg_opaque[yy * WIDE_W + ox]) /* behind the background */
                    continue;
                out[yy][ox] = p->pal[0x10 | pal_sel << 2 | pix] & 0x3F;
            }
        }
    }
}

static int draw_margin_sprites(const Nes *n, uint8_t out[PPU_H][WIDE_W], const uint8_t *widened,
                               const uint8_t *bg_opaque)
{
    int drawn = 0;
    int slot, camx = n->ram[0x8D] | n->ram[0x8E] << 8, camy = (int16_t)(n->ram[0x8F] | n->ram[0x90] << 8);
    if (!(n->ppu.mask & 0x10) || (n->ppu.ctrl & 0x20)) /* sprites off, or 8x16 sprites (not handled) */
        return 0;
    int votes[5] = {0}, ddy = 0, k;
    /* the game's own screen Y of the entities in the window ($0494) against ours: use the most common
     * difference (-2..2) for the margin entities, so a sprite crossing the window edge lines up */
    for (slot = 0; slot < 15; slot++) {
        int wx = n->ram[0x03FE + slot] | n->ram[0x03EF + slot] << 8, sx = (int16_t)(wx - camx), sy, d;
        if (!n->ram[0x03C2 + slot] || sx < 0 || sx >= PPU_W || !n->ram[0x0494 + slot])
            continue;
        sy = (int16_t)((n->ram[0x041C + slot] | n->ram[0x040D + slot] << 8) -
                       (n->ram[0x043A + slot] | n->ram[0x042B + slot] << 8) + n->ram[0x0476 + slot] - camy);
        d = (int8_t)(uint8_t)(n->ram[0x0494 + slot] - (sy & 0xFF));
        if (d >= -2 && d <= 2)
            votes[d + 2]++;
    }
    for (k = 0; k < 5; k++)
        if (votes[k] > votes[ddy + 2])
            ddy = k - 2;
    for (slot = 14; slot >= 0; slot--) { /* lower slots end up on top */
        int wx, sx, sy;
        if (!n->ram[0x03C2 + slot])
            continue;
        wx = n->ram[0x03FE + slot] | n->ram[0x03EF + slot] << 8;
        sx = (int16_t)(wx - camx);
        if (sx >= 0 && sx < PPU_W) /* in the window: the game drew it */
            continue;
        if (sx < -WIDE_MARGIN - 64 || sx >= PPU_W + WIDE_MARGIN + 64)
            continue;
        /* screen Y = depth - altitude (+ $0476) - camera Y, as bank 0 sub_8688 */
        sy = (int16_t)((n->ram[0x041C + slot] | n->ram[0x040D + slot] << 8) -
                       (n->ram[0x043A + slot] | n->ram[0x042B + slot] << 8) + n->ram[0x0476 + slot] - camy);
        sy += ddy;
        sy -= n->ram[0x0476 + slot]; /* the drawer subtracts the height again for the anchor */
        draw_entity(n, slot, sx, sy, out, widened, bg_opaque);
        drawn++;
    }
    return drawn;
}

#include <stdio.h>
#include <stdlib.h>
/* WIDE_DEBUG=1: which map column / row offset the nametable's middle column really shows (development aid) */
static void debug_dump(const Nes *n, const Level *L)
{
    const uint8_t *nt = n->ppu.nt[n->ppu.nt_page];
    int camx = n->ram[0x8D] | n->ram[0x8E] << 8, camy = (int16_t)(n->ram[0x8F] | n->ram[0x90] << 8);
    int tx = ((camx >> 3) + 16) & 31, bc, br, sub, best = 0, bbc = -1, bbr = -1, bsub = 0, off;
    fprintf(stderr, "wide debug: level %02X EA %02X cam %d,%d BE %d map $%04X blocks $%04X attr $%04X rows %d $89=%02X\n",
            n->ram[0x10], n->ram[0xEA], camx, camy, L->cols, L->map, L->blocks, L->attr, L->rows, n->ram[0x89]);
    /* nametable column tx, rows 0..29: find map column bc, tile column sub, row offset off with most matches */
    for (bc = 0; bc < L->cols; bc++)
        for (sub = 0; sub < 4; sub++)
            for (off = -40; off < 40; off++) {
                int ty, hit = 0;
                for (ty = 0; ty < 30; ty++) {
                    int wty = ty + off; /* world tile row */
                    uint8_t block;
                    br = wty >> 2;
                    if (wty < 0 || br >= L->rows)
                        continue;
                    block = byte_at(n, (uint16_t)(L->map + br * L->cols + bc));
                    if (byte_at(n, (uint16_t)(L->blocks + block * 16 + (wty & 3) * 4 + sub)) == nt[ty * 32 + tx])
                        hit++;
                }
                if (hit > best) {
                    best = hit;
                    bbc = bc;
                    bsub = sub;
                    bbr = off;
                }
            }
    fprintf(stderr, "wide debug: nt column %d best = map column %d tile %d (world tile col %d), row offset %d tiles, %d/30 rows\n",
            tx, bbc, bsub, bbc * 4 + bsub, bbr, best);
    fprintf(stderr, "wide debug: camera tile col +16 = %d\n", (camx >> 3) + 16);
    if (getenv("WIDE_DEBUG_COLS")) { /* every visible nametable column on its own */
        int c;
        for (c = 0; c < 32; c++) {
            int ntc = ((camx >> 3) + c) & 31, b2 = 0, bc2 = -1, bs2 = 0, bo2 = 0;
            for (bc = 0; bc < L->cols; bc++)
                for (sub = 0; sub < 4; sub++)
                    for (off = -40; off < 40; off++) {
                        int ty, hit = 0;
                        for (ty = 4; ty < 30; ty++) {
                            int wty = ty + off;
                            uint8_t block;
                            br = wty >> 2;
                            if (wty < 0 || br >= L->rows)
                                continue;
                            block = byte_at(n, (uint16_t)(L->map + br * L->cols + bc));
                            if (byte_at(n, (uint16_t)(L->blocks + block * 16 + (wty & 3) * 4 + sub)) == nt[ty * 32 + ntc])
                                hit++;
                        }
                        if (hit > b2) {
                            b2 = hit;
                            bc2 = bc;
                            bs2 = sub;
                            bo2 = off;
                        }
                    }
            fprintf(stderr, "wide debug: screen tile col %2d (world %d): map col %d tile %d row off %d, %d/26\n", c,
                    (camx >> 3) + c, bc2, bs2, bo2, b2);
        }
    }
    { /* the sprite pass's screen position vs the game's own ($0485, $0494 - $0476) for entities in the window */
        int slot, ok = 0, bad = 0;
        for (slot = 0; slot < 15; slot++) {
            int wx, sx, sy;
            if (!n->ram[0x03C2 + slot])
                continue;
            wx = n->ram[0x03FE + slot] | n->ram[0x03EF + slot] << 8;
            sx = (int16_t)(wx - camx);
            if (sx < 0 || sx >= 256)
                continue;
            sy = (int16_t)((n->ram[0x041C + slot] | n->ram[0x040D + slot] << 8) -
                           (n->ram[0x043A + slot] | n->ram[0x042B + slot] << 8) - camy);
            if ((sx & 0xFF) == n->ram[0x0485 + slot] && ((sy + n->ram[0x0476 + slot]) & 0xFF) == n->ram[0x0494 + slot])
                ok++;
            else {
                bad++;
                fprintf(stderr, "wide debug: slot %d type %02X x %d vs %d, y %d vs %d (h %d)\n", slot,
                        n->ram[0x03C2 + slot], sx & 0xFF, n->ram[0x0485 + slot], (sy + n->ram[0x0476 + slot]) & 0xFF,
                        n->ram[0x0494 + slot], n->ram[0x0476 + slot]);
            }
        }
        fprintf(stderr, "wide debug: entity screen positions: %d match the game's, %d differ\n", ok, bad);
    }
}

void wide_debug(const Nes *n)
{
    Level L = level_of(n);
    if (L.ok)
        debug_dump(n, &L);
}

void wide_to_rgb(const Screen *s, const uint8_t idx[PPU_H][WIDE_W], uint32_t *out, int pitch, int fade)
{
    int x, y;
    screen_index_to_rgb(&idx[0][0], WIDE_W, s->emph, WIDE_W, PPU_H, out, pitch);
    if (!fade)
        return;
    for (x = 0; x < 40; x++) { /* outer 40 pixels of each margin: 35% .. 100% */
        unsigned f = 90 + (unsigned)x * (256 - 90) / 40;
        for (y = 0; y < PPU_H; y++) {
            uint32_t *a = &out[y * pitch + x], *b = &out[y * pitch + WIDE_W - 1 - x];
            *a = 0xFF000000u | ((((*a >> 16) & 0xFF) * f >> 8) << 16) | ((((*a >> 8) & 0xFF) * f >> 8) << 8) |
                 ((*a & 0xFF) * f >> 8);
            *b = 0xFF000000u | ((((*b >> 16) & 0xFF) * f >> 8) << 16) | ((((*b >> 8) & 0xFF) * f >> 8) << 8) |
                 ((*b & 0xFF) * f >> 8);
        }
    }
}
