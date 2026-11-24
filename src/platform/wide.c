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

/* Background palette index of world pixel (wx, wy), or -1 outside the map. */
static int map_pixel(const Nes *n, const Level *L, int wx, int wy)
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
    if (!pix)
        return p->pal[0] & 0x3F;
    attr = byte_at(n, (uint16_t)(L->attr + block));
    q = ((wy >> 4) & 1) * 2 + ((wx >> 4) & 1);
    return p->pal[((attr >> (q * 2)) & 3) << 2 | pix] & 0x3F;
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

void wide_render(WideState *ws, const Nes *n, const Screen *s, uint8_t out[PPU_H][WIDE_W])
{
    Level L = level_of(n);
    int camx = n->ram[0x8D] | n->ram[0x8E] << 8;
    int camy = (int16_t)(n->ram[0x8F] | n->ram[0x90] << 8);
    int y, x, best = -1, best_dy = 0, best_dx = 0, dy;

    ws->lines_widened = 0;
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
        for (dx = 0; dx <= 64 && best < 950; dx = dx > 0 ? -dx : -dx + 1) {
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

    for (y = 0; y < PPU_H; y++) {
        int wy = camy + y + best_dy;
        if (line_match(n, &L, camx, wy, y, 2) < 900) /* status bar or a split: leave black */
            continue;
        ws->lines_widened++;
        for (x = -WIDE_MARGIN; x < 0; x++) {
            int m = map_pixel(n, &L, camx + x, wy);
            out[y][WIDE_MARGIN + x] = (uint8_t)(m < 0 ? 0x0F : m);
        }
        if (!(n->ppu.mask & 0x02)) /* the game hides its leftmost 8 pixels: fill them too, no seam */
            for (x = 0; x < 8; x++) {
                int m = map_pixel(n, &L, camx + x, wy);
                if (m >= 0)
                    out[y][WIDE_MARGIN + x] = (uint8_t)m;
            }
        for (x = PPU_W; x < PPU_W + WIDE_MARGIN; x++) {
            int m = map_pixel(n, &L, camx + x, wy);
            out[y][WIDE_MARGIN + x] = (uint8_t)(m < 0 ? 0x0F : m);
        }
    }
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
}

void wide_debug(const Nes *n)
{
    Level L = level_of(n);
    if (L.ok)
        debug_dump(n, &L);
}

void wide_to_rgb(const Screen *s, const uint8_t idx[PPU_H][WIDE_W], uint32_t *out, int pitch)
{
    screen_index_to_rgb(&idx[0][0], WIDE_W, s->emph, WIDE_W, PPU_H, out, pitch);
}
