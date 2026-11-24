#include "screen.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846

/* ---- line building ---- */

static int no_limit; /* M7 mod "nolimit" */

void screen_set_no_sprite_limit(int on)
{
    no_limit = on != 0;
}

/* Every sprite on display line y in OAM order, evaluated the way the old-model PPU picks its 8 (at the end of
 * line y-1, from the same OAM), without the limit. */
static int eval_all_sprites(const Ppu *p, int y, PpuSprite *out)
{
    unsigned height = (p->ctrl & 0x20) ? 16 : 8;
    unsigned table8 = ((p->ctrl & 0x28) == 0x08) ? 0x1000 : 0;
    int n, count = 0;
    for (n = 0; n < 64; n++) {
        const uint8_t *s = p->oam + n * 4;
        int row = y - 1 - s[0];
        unsigned addr;
        if ((unsigned)row >= height)
            continue;
        if (height == 16)
            addr = ((s[1] & 1u) << 12) + ((s[1] & 0xFEu) << 4);
        else
            addr = ((unsigned)s[1] << 4) + table8;
        if (s[2] & 0x80)
            addr = addr + 7 - (unsigned)row + ((p->ctrl & 0x20) >> 1) - ((unsigned)row & 8);
        else
            addr = addr + (unsigned)row + ((unsigned)row & 8);
        out[count].lo = p->chr[addr & 0x1FFF];
        out[count].hi = p->chr[(addr + 8) & 0x1FFF];
        out[count].attr = s[2];
        out[count].x = s[3];
        count++;
    }
    return count;
}

/* Sprite pixels of list[first..count) into spr[] (0x80 = none, bit 6 = behind background), drawn highest index
 * first so the lowest index wins. */
static void draw_sprites(const Ppu *p, const PpuSprite *list, int first, int count, uint8_t *spr)
{
    int n;
    for (n = count - 1; n >= first; n--) {
        const PpuSprite *s = &list[n];
        uint8_t opaque = s->lo | s->hi;
        int i;
        if (!opaque)
            continue;
        for (i = 0; i < 8; i++) {
            uint8_t bit = (s->attr & 0x40) ? (uint8_t)(1 << i) : (uint8_t)(0x80 >> i);
            int pix;
            uint8_t c;
            if (!(opaque & bit))
                continue;
            pix = ((s->lo & bit) ? 1 : 0) | ((s->hi & bit) ? 2 : 0);
            c = p->pal[0x10 | (s->attr & 3) << 2 | pix];
            if (s->attr & 0x20)
                c |= 0x40;
            spr[s->x + i] = c;
        }
    }
}

/* Old model: sprites drawn highest index first so the lowest index wins; a behind-background sprite pixel
 * only shows where the background is transparent (fb bit 6). Left 8 pixels hidden by PPUMASK bit 2.
 * With "nolimit" every sprite on the line is drawn, not only the PPU's 8. */
static void compose_old(Screen *scr, const Ppu *p, int y, uint8_t *out)
{
    static PpuSprite all[64];
    const uint8_t *bg = p->fb + y * PPU_W;
    const PpuSprite *list = p->line_sprites;
    uint8_t spr[PPU_W + 8];
    int x, n, count = p->line_sprite_count;

    if (no_limit && (p->mask & 0x10)) {
        int k = eval_all_sprites(p, y, all), same = k >= count;
        for (n = 0; same && n < count; n++)
            same = !memcmp(&all[n], &p->line_sprites[n], sizeof all[n]);
        if (!same) {
            /* the sprite data changed between the PPU's fetch (end of line y-1) and now (seen when the game
             * rewrites a sprite tile mid-frame): show the PPU's 8, as without the mod */
            scr->sprite_mismatch_lines++;
        } else {
            if (k > count) {
                scr->extra_sprite_lines++;
                scr->line_extra[y] = 1;
            }
            list = all;
            count = k;
        }
    }
    for (x = 0; x < PPU_W; x++)
        out[x] = bg[x];
    if (!(p->mask & 0x10) || !count)
        goto finish;
    memset(spr, 0x80, sizeof spr);
    draw_sprites(p, list, 0, count, spr);
    for (x = (p->mask & 0x04) ? 0 : 8; x < PPU_W; x++) {
        uint8_t t = spr[x];
        if (!(t & 0x80) && (!(t & 0x40) || (bg[x] & 0x40)))
            out[x] = t;
    }
finish:
    for (x = 0; x < PPU_W; x++)
        out[x] &= 0x3F;
    if ((p->mask & 0x18) && (p->mask & 0x01))
        for (x = 0; x < PPU_W; x++)
            out[x] &= 0x30;
}

/* New model with "nolimit": the PPU's picture already holds the first 8 sprites; the others are added where it
 * shows the backdrop color (the dot model keeps no background-transparency bit, so a background pixel that
 * happens to use the backdrop color counts as transparent here). */
static void add_extra_sprites_new(Screen *scr, const Ppu *p, int y, uint8_t *out)
{
    static PpuSprite all[64];
    uint8_t spr[PPU_W + 8], backdrop = p->pal[0] & 0x3F, grey = (p->mask & 0x01) ? 0x30 : 0x3F;
    int count, x;
    if (!(p->mask & 0x10))
        return;
    count = eval_all_sprites(p, y, all);
    if (count <= 8)
        return;
    scr->extra_sprite_lines++;
    scr->line_extra[y] = 1;
    memset(spr, 0x80, sizeof spr);
    draw_sprites(p, all, 8, count, spr);
    for (x = (p->mask & 0x04) ? 0 : 8; x < PPU_W; x++)
        if (!(spr[x] & 0x80) && out[x] == (backdrop & grey))
            out[x] = (uint8_t)(spr[x] & grey);
}

static void on_line(Ppu *p, int y, void *user)
{
    Screen *s = user;
    if (y < 0 || y >= PPU_H)
        return;
    s->line_extra[y] = 0;
    if (p->model == PPU_MODEL_OLD) {
        compose_old(s, p, y, s->idx[y]);
    } else {
        int x;
        for (x = 0; x < PPU_W; x++)
            s->idx[y][x] = p->fb[y * PPU_W + x] & 0x3F;
        if (no_limit)
            add_extra_sprites_new(s, p, y, s->idx[y]);
    }
    s->emph[y] = (uint8_t)(p->mask >> 5);
}

void screen_attach(Screen *s, Ppu *ppu)
{
    memset(s, 0, sizeof *s);
    ppu->line_done = on_line;
    ppu->line_done_user = s;
}

/* ---- palette ----
 * Generated by simulating the PPU's composite video signal and decoding it as NTSC YIQ: each color is a
 * square wave between two voltage levels, 12 phases per color-burst cycle; hue selects the phase, the
 * level the voltages; emphasis attenuates the signal during the phases of the emphasized colors. */
void screen_palette(uint32_t rgb[8][64])
{
    static const double lo[4] = {0.350, 0.518, 0.962, 1.550};
    static const double hi[4] = {1.094, 1.506, 1.962, 1.962};
    const double black = 0.518, white = 1.962, atten = 0.746;
    int emph, c, p;

    for (emph = 0; emph < 8; emph++) {
        for (c = 0; c < 64; c++) {
            int hue = c & 15, level = (c >> 4) & 3;
            double y = 0, i = 0, q = 0, r, g, b;
            for (p = 0; p < 12; p++) {
                double low = lo[level], high = hi[level], v;
                int on = ((hue + p) % 12) < 6;
                if (hue == 0)
                    low = high;
                else if (hue == 13)
                    high = low;
                else if (hue > 13)
                    low = high = black;
                v = on ? high : low;
                /* emphasis bits: red at phase 0..5 band of hue 12, green of hue 4, blue of hue 8 */
                if (((emph & 1) && ((12 + p) % 12) < 6) || ((emph & 2) && ((4 + p) % 12) < 6) ||
                    ((emph & 4) && ((8 + p) % 12) < 6))
                    if (hue < 14)
                        v *= atten;
                v = (v - black) / (white - black);
                y += v / 12;
                i += v * cos(PI * (p + 4) / 6) / 12; /* +4 phases: color burst reference */
                q += v * sin(PI * (p + 4) / 6) / 12;
            }
            i *= 1.5; /* saturation: closer to the usual emulator palettes */
            q *= 1.5;
            r = y + 0.946882 * i + 0.623557 * q;
            g = y - 0.274788 * i - 0.635691 * q;
            b = y - 1.108545 * i + 1.709007 * q;
#define CH(v) ((uint32_t)((v) < 0 ? 0 : (v) > 1 ? 255 : (int)(pow((v), 1.0 / 1.15) * 255 + 0.5)))
            rgb[emph][c] = CH(r) << 16 | CH(g) << 8 | CH(b);
#undef CH
        }
    }
}

static uint32_t active_pal[8][64];
static int pal_ready;

int screen_set_palette(const uint8_t *rgb, int count)
{
    int e, c;
    if (count != 64 && count != 512)
        return -1;
    for (e = 0; e < 8; e++)
        for (c = 0; c < 64; c++) {
            const uint8_t *p = rgb + 3 * (count == 512 ? e * 64 + c : c);
            double r = p[0], g = p[1], b = p[2];
            if (count == 64 && e) { /* emphasis: emphasized channels keep their level, the others dim */
                if (!(e & 1))
                    r *= 0.816;
                if (!(e & 2))
                    g *= 0.816;
                if (!(e & 4))
                    b *= 0.816;
            }
            active_pal[e][c] = (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        }
    pal_ready = 1;
    return 0;
}

void screen_to_rgb(const Screen *s, uint32_t *out, int pitch)
{
    uint32_t(*pal)[64] = active_pal;
    int x, y;
    if (!pal_ready) {
        screen_palette(active_pal);
        pal_ready = 1;
    }
    for (y = 0; y < PPU_H; y++)
        for (x = 0; x < PPU_W; x++)
            out[y * pitch + x] = 0xFF000000u | pal[s->emph[y] & 7][s->idx[y][x]];
}
