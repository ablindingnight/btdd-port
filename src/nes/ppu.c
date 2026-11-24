/* PPU at M1 level. See ppu.h for the model. Dots per scanline: 341 (256 visible + 85). */
#include "ppu.h"
#include "../runtime/cpu.h"
#include <string.h>

#define TRANSPARENT 0x40 /* bit set in fb for a background pixel of color 0 */
#define RENDERING(p) ((p)->mask & 0x18)
#define BG_ON(p) ((p)->mask & 0x08)
#define SPR_ON(p) ((p)->mask & 0x10)
#define LINE_END_PIXEL 272 /* "pixel" index used to finish a line: 34 tiles incl. 2 prefetched */
#define FIX_PIXEL (LINE_END_PIXEL - 4)

static int64_t now16(const Ppu *p)
{
    /* Current CPU time in 1/16 dots, on the same scale as line_start. */
    return (int64_t)p->cpu->cycles * CPU_CYCLE_UNITS;
}

static int64_t sched16(const Ppu *p)
{
    /* The scheduled time (where the frame schedule is), independent of instruction overshoot. */
    return (int64_t)p->cpu->cycles * CPU_CYCLE_UNITS + p->cpu->budget;
}

static int floor_div16(int64_t x)
{
    return (int)(x >= 0 ? x / 16 : -((-x + 15) / 16));
}

/* ---- VRAM address increments during rendering ---- */

static void inc_vertical(Ppu *p)
{
    uint32_t v = p->v;
    if ((v & 0x7000) == 0x7000) {
        v ^= 0x7000;
        if ((v & 0x3E0) == 0x3A0)
            v ^= 0xBA0;
        else if ((v & 0x3E0) == 0x3E0)
            v ^= 0x3E0;
        else
            v += 0x20;
    } else {
        v += 0x1000;
    }
    p->v = v;
}

static void fix_vertical(Ppu *p)
{
    if (RENDERING(p))
        inc_vertical(p);
}

static void copy_horizontal(Ppu *p)
{
    if (RENDERING(p))
        p->v = (p->v & 0xFBE0) | (p->t & 0x041F);
}

/* ---- background line ---- */

static void check_sprite_hit(Ppu *p, int lastpixel)
{
    int limit = lastpixel - 16, x;
    const uint8_t *line = p->fb + p->line_base;

    if (p->hit_x == 0x100)
        return;
    for (x = p->hit_x; x < p->hit_x + 8 && x < limit; x++) {
        if (x < 255 && (p->hit_mask & (0x80 >> (x - p->hit_x))) && !(line[x] & TRANSPARENT)) {
            p->status |= 0x40;
            p->hit_x = 0x100;
            break;
        }
    }
}

static void fill(Ppu *p, int at, int count)
{
    memset(p->fb + at, p->pal[0] | TRANSPARENT, (size_t)count);
}

/* Draws tiles first_tile..(lastpixel/8) of the current line. Tiles 0 and 1 are the two tiles the
 * PPU prefetches at the end of the previous line; pixels start with tile 2. */
static void refresh_line(Ppu *p, int lastpixel)
{
    int lasttile = lastpixel >> 3, tile;
    uint32_t va;
    unsigned fine_y_base;

    /* Pending sprite-0 hit: draw one tile ahead so the hit pixel is present. */
    if (p->hit_x != 0x100 && !(p->status & 0x40)) {
        if (p->hit_x < lastpixel - 16 && !(p->hit_x < (lasttile - 2) * 8))
            lasttile++;
    }
    if (lasttile > 34)
        lasttile = 34;
    if (lasttile - p->first_tile <= 0)
        return;

    if (!RENDERING(p)) {
        int count = (lasttile - p->first_tile) * 8;
        fill(p, p->line_pos, count);
        p->line_pos += count;
        p->first_tile = lasttile;
        if (lastpixel >= FIX_PIXEL && p->fix_pending) {
            fix_vertical(p);
            p->fix_pending = 0;
        }
        return;
    }

    va = p->v;
    fine_y_base = ((p->ctrl & 0x10u) << 8) | ((va >> 12) & 7);
    for (tile = p->first_tile; tile < lasttile; tile++) {
        const uint8_t *nt = p->nt[p->nt_page];
        unsigned name_addr, attr, coarse_x = va & 0x1F;

        if (tile >= 2) {
            uint8_t *out = p->fb + p->line_pos;
            unsigned lo = (p->pshift[0] >> (8 - p->fine_x)) & 0xFF;
            unsigned hi = (p->pshift[1] >> (8 - p->fine_x)) & 0xFF;
            int i;
            for (i = 0; i < 8; i++) {
                unsigned pix = ((lo >> (7 - i)) & 1) | (((hi >> (7 - i)) & 1) << 1);
                unsigned pal_sel = (p->at_latch >> (((i + p->fine_x) >> 3) * 2)) & 3;
                out[i] = pix ? p->pal[pal_sel << 2 | pix] : (uint8_t)(p->pal[0] | TRANSPARENT);
            }
            p->line_pos += 8;
        }

        name_addr = ((unsigned)nt[va & 0x3FF] << 4) + fine_y_base;
        attr = nt[0x3C0 + (coarse_x >> 2) + ((va & 0x380) >> 4)];
        attr = (attr >> ((coarse_x & 2) + ((va & 0x40) >> 4))) & 3;
        p->at_latch = (p->at_latch >> 2) | (attr << 2);
        p->pshift[0] = (p->pshift[0] << 8) | p->chr[name_addr & 0x1FFF];
        p->pshift[1] = (p->pshift[1] << 8) | p->chr[(name_addr + 8) & 0x1FFF];

        if ((va & 0x1F) == 0x1F)
            va ^= 0x41F;
        else
            va++;
    }
    p->v = va;

    /* Left 8 pixels hidden by PPUMASK bit 1. */
    if (p->first_tile <= 2 && 2 < lasttile && !(p->mask & 0x02))
        fill(p, p->line_base, 8);

    if (!BG_ON(p)) {
        int start = p->first_tile - 2, count = lasttile - p->first_tile;
        if (start < 0) {
            count += start;
            start = 0;
        }
        if (count > 0)
            fill(p, p->line_base + start * 8, count * 8);
    }

    if (lastpixel >= FIX_PIXEL && p->fix_pending) {
        fix_vertical(p);
        p->fix_pending = 0;
    }
    check_sprite_hit(p, lastpixel);
    p->first_tile = lasttile;
}

void ppu_line_update(Ppu *p)
{
    if (p->line_active)
        refresh_line(p, floor_div16(now16(p) - p->line_start));
}

static void begin_line(Ppu *p, int y)
{
    p->line_base = y * PPU_W;
    memset(p->fb + p->line_base, 0xFF, PPU_W);
    p->line_pos = p->line_base;
    p->first_tile = 0;
    p->line_start = sched16(p);
    p->line_active = 1;
    p->fix_pending = 0;
    ppu_line_update(p);
    p->fix_pending = 1;
}

static void end_line(Ppu *p)
{
    refresh_line(p, LINE_END_PIXEL);
    if (p->fix_pending)
        fix_vertical(p);
    check_sprite_hit(p, LINE_END_PIXEL);
    p->line_active = 0;
}

/* ---- sprites ---- */

static uint8_t reverse8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

/* Sprite evaluation for the next line (done at the end of the current one). */
static void fetch_sprites(Ppu *p)
{
    unsigned height = (p->ctrl & 0x20) ? 16 : 8;
    unsigned table8 = ((p->ctrl & 0x28) == 0x08) ? 0x1000 : 0;
    int n, count = 0;

    p->sprite0_on_line = 0;
    for (n = 0; n < 64; n++) {
        const uint8_t *s = p->oam + n * 4;
        int row = p->scanline - s[0];
        unsigned addr;
        PpuSprite *dst;

        if ((unsigned)row >= height)
            continue;
        if (count >= 8) {
            p->status |= 0x20;
            break;
        }
        if (n == 0)
            p->sprite0_on_line = 1;
        if (height == 16)
            addr = ((s[1] & 1u) << 12) + ((s[1] & 0xFEu) << 4);
        else
            addr = ((unsigned)s[1] << 4) + table8;
        if (s[2] & 0x80)
            addr = addr + 7 - (unsigned)row + ((p->ctrl & 0x20) >> 1) - ((unsigned)row & 8);
        else
            addr = addr + (unsigned)row + ((unsigned)row & 8);
        dst = &p->line_sprites[count++];
        dst->lo = p->chr[addr & 0x1FFF];
        dst->hi = p->chr[(addr + 8) & 0x1FFF];
        dst->attr = s[2];
        dst->x = s[3];
    }
    p->line_sprite_count = count;
}

/* Arms the sprite-0 hit check for the next line. */
static void prepare_sprites(Ppu *p)
{
    if (p->line_sprite_count) {
        const PpuSprite *s = &p->line_sprites[0];
        uint8_t opaque = s->lo | s->hi;
        if (opaque && p->sprite0_on_line && !(p->status & 0x40)) {
            p->hit_x = s->x;
            p->hit_mask = (s->attr & 0x40) ? reverse8(opaque) : opaque;
        }
        p->sprite0_on_line = 0;
    }
}

/* ---- registers ---- */

static uint8_t vram_read(Ppu *p, uint32_t a)
{
    a &= 0x3FFF;
    if (a < 0x2000)
        return p->chr[a];
    return p->nt[p->nt_page][a & 0x3FF];
}

uint8_t ppu_read(Ppu *p, uint16_t reg)
{
    uint8_t ret;

    switch (reg & 7) {
    case 2:
        ppu_line_update(p);
        ret = (uint8_t)(p->status | (p->latch & 0x1F));
        p->toggle = 0;
        p->status &= 0x7F;
        p->latch = ret;
        return ret;
    case 7: {
        uint32_t a = p->v & 0x3FFF;
        ppu_line_update(p);
        if (a >= 0x3F00) {
            if (!(a & 3))
                ret = (a & 0xC) ? p->pal_hidden[((a & 0xC) >> 2) - 1] : p->pal[0];
            else
                ret = p->pal[a & 0x1F];
            if (p->mask & 0x01) /* grayscale */
                ret &= 0x30;
            p->read_buf = vram_read(p, a - 0x1000);
        } else {
            ret = p->read_buf;
            p->latch = p->read_buf;
            p->read_buf = vram_read(p, a);
        }
        if (RENDERING(p) && p->scanline < 240)
            inc_vertical(p);
        else
            p->v += (p->ctrl & 0x04) ? 32 : 1;
        return ret;
    }
    default: /* write-only registers (and $2004 in this model) return the latch */
        ppu_line_update(p);
        return p->latch;
    }
}

void ppu_write(Ppu *p, uint16_t reg, uint8_t v)
{
    switch (reg & 7) {
    case 0:
        ppu_line_update(p);
        p->latch = v;
        if (!(p->ctrl & 0x80) && (v & 0x80) && (p->status & 0x80))
            p->cpu->irq |= CPU_INT_NMI2;
        p->ctrl = v;
        p->t = (p->t & 0xF3FF) | ((uint32_t)(v & 3) << 10);
        break;
    case 1:
        ppu_line_update(p);
        p->latch = v;
        p->mask = v;
        break;
    case 2:
        p->latch = v;
        break;
    case 3:
        p->latch = v;
        p->oam_addr = v;
        p->oam_low = v & 7;
        break;
    case 4:
        p->latch = v;
        if (p->oam_low >= 8) {
            if (p->oam_addr >= 8)
                p->oam[p->oam_addr] = v;
        } else {
            p->oam[p->oam_low] = v;
        }
        p->oam_addr++;
        p->oam_low++;
        break;
    case 5:
        ppu_line_update(p);
        p->latch = v;
        if (!p->toggle) {
            p->t = (p->t & 0xFFE0) | (v >> 3);
            p->fine_x = v & 7;
        } else {
            p->t = (p->t & 0x8C1F) | ((uint32_t)(v & 0xF8) << 2) | ((uint32_t)(v & 7) << 12);
        }
        p->toggle ^= 1;
        break;
    case 6:
        ppu_line_update(p);
        p->latch = v;
        if (!p->toggle) {
            p->t = (p->t & 0x00FF) | ((uint32_t)(v & 0x3F) << 8);
        } else {
            p->t = (p->t & 0xFF00) | v;
            p->v = p->t;
        }
        p->toggle ^= 1;
        break;
    case 7: {
        uint32_t a = p->v & 0x3FFF;
        p->latch = v;
        if (a < 0x2000) {
            p->chr[a] = v;
        } else if (a < 0x3F00) {
            p->nt[p->nt_page][a & 0x3FF] = v;
        } else if (!(a & 3)) {
            if (!(a & 0xC))
                p->pal[0] = p->pal[4] = p->pal[8] = p->pal[12] = v & 0x3F;
            else
                p->pal_hidden[((a & 0xC) >> 2) - 1] = v & 0x3F;
        } else {
            p->pal[a & 0x1F] = v & 0x3F;
        }
        p->v += (p->ctrl & 0x04) ? 32 : 1;
        break;
    }
    }
}

/* ---- frame schedule ---- */

static void visible_line(Ppu *p)
{
    cpu_run(p->cpu, 256);
    end_line(p);
    p->hit_x = 0x100;
    if (RENDERING(p))
        fetch_sprites(p);
    cpu_run(p->cpu, 6);
    copy_horizontal(p);
    cpu_run(p->cpu, 85 - 6 - 16);
    if (SPR_ON(p))
        prepare_sprites(p);
    p->scanline++;
    if (p->scanline < 240)
        begin_line(p, p->scanline);
    cpu_run(p->cpu, 16);
}

void ppu_run_frame(Ppu *p)
{
    p->frame++;
    if (p->dead_frames) {
        memset(p->fb, 0x80, PPU_W * PPU_H);
        cpu_run(p->cpu, 262 * 341);
        p->dead_frames--;
        return;
    }

    /* line 240 (post-render), then vblank: flag set at the start of line 241 */
    cpu_run(p->cpu, 341);
    p->status |= 0x80;
    p->oam_addr = p->oam_low = 0;
    cpu_run(p->cpu, 12);
    if (p->ctrl & 0x80)
        p->cpu->irq |= CPU_INT_NMI;
    cpu_run(p->cpu, 20 * 341 - 12);

    /* pre-render line */
    p->status &= 0x1F;
    cpu_run(p->cpu, 256);
    cpu_run(p->cpu, 85 - 16);
    if (RENDERING(p))
        p->v = p->t;
    p->line_sprite_count = 0;
    begin_line(p, 0);
    cpu_run(p->cpu, 16 - p->odd);
    p->odd ^= 1;

    for (p->scanline = 0; p->scanline < 240;)
        visible_line(p);
}

void ppu_power(Ppu *p, struct Cpu *cpu)
{
    int i;
    memset(p, 0, sizeof *p);
    p->cpu = cpu;
    /* FCEUX power-on contents: CIRAM/palette/OAM zero, CHR-RAM in its 00 00 00 00 FF FF FF FF pattern */
    for (i = 0; i < 0x2000; i++)
        p->chr[i] = (i & 4) ? 0xFF : 0x00;
    p->dead_frames = 2;
    p->hit_x = 0;     /* FCEUX's statics start at 0, not at "no hit pending" */
    p->scanline = 0;
}
