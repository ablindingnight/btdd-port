/* PPU, dot-based model: behaves like FCEUX 2.6's "new PPU" (newppu 1), which the 1P TAS needs.
 *
 * The frame starts at vblank. Every PPU dot gives the CPU one dot of time (cpu_run(1)); background tiles
 * are fetched 2 dots per memory access exactly where the frame schedule says, scroll counters are
 * clocked at the fetch points, and pixels (with sprite-0 hit) are produced 8 at a time right after each
 * tile fetch. Written from FCEUX's behaviour, not its code. */
#include "ppu_internal.h"
#include "../runtime/cpu.h"
#include <string.h>

#define PPU_ON(p) ((p)->mask & 0x18)
#define LINE_DOTS 341

static void run(Ppu *p, int dots)
{
    p->dot = (p->dot + dots) % p->end_dot;
    cpu_run(p->cpu, dots);
}

/* ---- scroll counters ---- */

static void install_latches(PpuScroll *s)
{
    s->fv = s->lfv;
    s->v = s->lv;
    s->h = s->lh;
    s->vt = s->lvt;
    s->ht = s->lht;
}

static void inc_horizontal(PpuScroll *s)
{
    s->ht++;
    s->h += s->ht >> 5;
    s->ht &= 31;
    s->h &= 1;
}

static void inc_vertical(PpuScroll *s)
{
    uint32_t carry;
    s->fv++;
    carry = s->fv >> 3;
    s->vt += carry;
    s->vt &= 31;
    if (s->vt == 30 && carry == 1) {
        s->v++;
        s->vt = 0;
    }
    s->fv &= 7;
    s->v &= 1;
}

static uint32_t nt_addr(const PpuScroll *s)
{
    return 0x2000 | (s->v << 11) | (s->h << 10) | (s->vt << 5) | s->ht;
}

static uint32_t at_addr(const PpuScroll *s)
{
    return 0x2000 | (s->v << 11) | (s->h << 10) | 0x3C0 | ((s->vt & 0x1C) << 1) | ((s->ht & 0x1C) >> 2);
}

static uint32_t data_addr(const PpuScroll *s)
{
    return ((s->fv & 3) << 12) | (s->v << 11) | (s->h << 10) | (s->vt << 5) | s->ht;
}

/* $2007 access: during rendering it clocks the vertical counter, otherwise the 15-bit chain. */
static void inc_data(Ppu *p)
{
    PpuScroll *s = &p->sc;
    if (p->sl >= 0 && p->sl < 241 && PPU_ON(p)) {
        inc_vertical(s);
        return;
    }
    if (p->ctrl & 0x04) {
        s->vt++;
    } else {
        s->ht++;
        s->vt += (s->ht >> 5) & 1;
    }
    s->h += s->vt >> 5;
    s->v += s->h >> 1;
    s->fv += s->v >> 1;
    s->ht &= 31;
    s->vt &= 31;
    s->h &= 1;
    s->v &= 1;
    s->fv &= 7;
}

/* ---- VRAM ---- */

static uint8_t pal_read(const Ppu *p, uint32_t a)
{
    uint8_t ret;
    if (!(a & 3))
        ret = (a & 0xC) ? p->pal_hidden[((a & 0xC) >> 2) - 1] : p->pal[0];
    else
        ret = p->pal[a & 0x1F];
    return (p->mask & 0x01) ? (uint8_t)(ret & 0x30) : ret;
}

static uint8_t vread(const Ppu *p, uint32_t a)
{
    a &= 0x3FFF;
    if (a < 0x2000)
        return p->chr[a];
    if (a < 0x3F00)
        return p->nt[p->nt_page][a & 0x3FF];
    return pal_read(p, a);
}

/* ---- register paths that differ from the old model ---- */

uint8_t ppun_read_data(Ppu *p)
{
    uint32_t old = p->v & 0x3FFF; /* FCEUX picks palette entries from the previous address register */
    uint32_t a = data_addr(&p->sc) & 0x3FFF;
    uint8_t ret = p->read_buf;

    p->v = a;
    if ((a & 0x3F00) == 0x3F00) {
        ret = pal_read(p, old);
        p->read_buf = vread(p, a - 0x1000);
    } else {
        p->read_buf = vread(p, a);
    }
    inc_data(p);
    p->v = data_addr(&p->sc);
    return ret;
}

void ppun_write_data(Ppu *p, uint8_t v)
{
    uint32_t a = data_addr(&p->sc) & 0x3FFF;
    p->latch = v;
    p->v = a;
    if (a < 0x2000) {
        p->chr[a] = v;
    } else if (a < 0x3F00) {
        p->nt[p->nt_page][a & 0x3FF] = v;
    } else if (!(a & 3)) {
        if (!(a & 0xC)) {
            int i;
            for (i = 0; i < 32; i += 4)
                p->pal[i] = v & 0x3F;
        } else {
            p->pal_hidden[((a & 0xC) >> 2) - 1] = v & 0x3F;
        }
    } else {
        p->pal[a & 0x1F] = v & 0x3F;
    }
    inc_data(p);
    p->v = data_addr(&p->sc);
}

uint8_t ppun_read_oam(Ppu *p)
{
    /* FCEUX emulates the sprite-evaluation bus here during rendering; BTDD never reads $2004, so the
     * rendering case only returns the plain OAM byte and is counted. */
    if (p->sl < 241 && PPU_ON(p))
        p->oam_reads_rendering++;
    return p->oam[p->oam_addr];
}

void ppun_write_oam(Ppu *p, uint8_t v)
{
    p->latch = v;
    if ((p->oam_addr & 3) == 2)
        v &= 0xE3;
    p->oam[p->oam_addr] = v;
    p->oam_addr++;
}

void ppun_write_scroll(Ppu *p, int reg, uint8_t v, int second)
{
    PpuScroll *s = &p->sc;
    switch (reg) {
    case 0:
        s->lh = v & 1;
        s->lv = (v >> 1) & 1;
        s->s = (v >> 4) & 1;
        break;
    case 5:
        if (!second) {
            s->lht = v >> 3;
            s->fh = v & 7;
        } else {
            s->lvt = v >> 3;
            s->lfv = v & 7;
        }
        break;
    case 6:
        if (!second) {
            s->lvt = (s->lvt & 7) | ((uint32_t)(v & 3) << 3);
            s->lh = (v >> 2) & 1;
            s->lv = (v >> 3) & 1;
            s->lfv = (v >> 4) & 3;
        } else {
            s->lvt = (s->lvt & 0x18) | (v >> 5);
            s->lht = v & 31;
            install_latches(s);
        }
        break;
    }
}

/* ---- frame ---- */

static void fetch_tile(Ppu *p, PpuBgTile *t)
{
    PpuScroll *s = &p->sc;
    uint8_t nt, at;
    uint32_t a;

    p->v = nt_addr(s);
    nt = vread(p, p->v);
    run(p, 1);
    run(p, 1);
    p->v = at_addr(s);
    at = vread(p, p->v);
    if (s->vt & 2)
        at >>= 4;
    if (s->ht & 2)
        at >>= 2;
    t->at = (uint8_t)((at & 3) << 2);
    run(p, 1);
    if (PPU_ON(p)) {
        inc_horizontal(s);
        if (p->dot == 251)
            inc_vertical(s);
    }
    run(p, 1);
    s->par = nt;
    a = (s->s << 12) | (s->par << 4) | s->fv;
    p->v = a;
    t->pt[0] = vread(p, a);
    run(p, 1);
    run(p, 1);
    p->v = a | 8;
    t->pt[1] = vread(p, a | 8);
    run(p, 1);
    run(p, 1);
}

static void draw_tile(Ppu *p, int yp, int xt, int renderslot)
{
    int oamcount = p->oam_counts[renderslot];
    int spr_on = (p->mask & 0x10) && (xt > 0 || (p->mask & 0x04));
    int bg_on = (p->mask & 0x08) && (xt > 0 || (p->mask & 0x02));
    uint8_t *out = p->fb + yp * PPU_W + xt * 8;
    int xp;

    for (xp = 0; xp < 8; xp++) {
        int x = xt * 8 + xp, i, have = 0;
        int bgpos = x + (int)p->sc.fh;
        uint8_t pixel = 0, color;

        if (bg_on) {
            const PpuBgTile *t = &p->bg[bgpos >> 3];
            int bit = 7 - (bgpos & 7);
            pixel = (uint8_t)(((t->pt[0] >> bit) & 1) | (((t->pt[1] >> bit) & 1) << 1) | t->at);
        }
        color = p->pal[pixel];
        for (i = 0; i < oamcount; i++) {
            uint8_t *o = p->oams[renderslot][i];
            uint8_t sp;
            if (x < o[3] || x >= o[3] + 8)
                continue;
            sp = (uint8_t)((o[4] & 1) | ((o[5] & 1) << 1));
            o[4] >>= 1;
            o[5] >>= 1;
            if (!spr_on || have || !sp)
                continue;
            if (o[6] == 0 && (pixel & 3) && x < 255)
                p->status |= 0x40;
            have = 1;
            if ((o[2] & 0x20) && (pixel & 3))
                continue;
            color = p->pal[0x10 | sp | (o[2] & 3) << 2];
        }
        if (p->mask & 0x01)
            color &= 0x30;
        out[xp] = color;
    }
}

static uint8_t reverse8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

static void evaluate_sprites(Ppu *p, int yp, int slot)
{
    int height = (p->ctrl & 0x20) ? 16 : 8, i, count = 0;

    for (i = 0; i < 64; i++) {
        const uint8_t *spr = p->oam + i * 4;
        if (count < 64)
            p->oams[slot][count][7] = 0;
        if (yp >= spr[0] && yp < spr[0] + height) {
            if (count >= 8 && PPU_ON(p)) {
                p->status |= 0x20;
                break;
            }
            if (count < 64) {
                memcpy(p->oams[slot][count], spr, 4);
                p->oams[slot][count][7] = 1;
                p->oams[slot][count][6] = (uint8_t)i;
            }
            count++;
        }
    }
    p->oam_counts[slot] = count > 64 ? 64 : count;
}

static void fetch_sprites(Ppu *p, int sl, int yp, int slot)
{
    int height = (p->ctrl & 0x20) ? 16 : 8, n;

    for (n = 0; n < 8; n++) {
        uint8_t *o = p->oams[slot][n];
        uint32_t line = (uint32_t)(yp - o[0]), tile = o[1], addr;
        int garbage = 2;

        if (o[2] & 0x80)
            line = (uint32_t)height - line - 1;
        if (!o[7]) {
            tile = 0;
            line = 0;
        }
        if (height == 16)
            addr = (((tile & ~1u) | (line >> 3)) << 4) | ((tile & 1) << 12);
        else
            addr = (tile << 4) | ((uint32_t)(p->ctrl & 0x08) << 9);
        addr += line & 7;

        if (PPU_ON(p)) {
            if (sl == 0 && p->dot == 304) {
                run(p, 1);
                if (PPU_ON(p))
                    install_latches(&p->sc);
                run(p, 1);
                garbage = 0;
            }
            if (sl != 0 && sl < 241 && p->dot == 256) {
                run(p, 1);
                if (PPU_ON(p)) {
                    p->sc.ht = p->sc.lht;
                    p->sc.h = p->sc.lh;
                }
                run(p, 1);
                garbage = 0;
            }
        }
        if (garbage)
            run(p, garbage);
        run(p, 2);
        p->v = addr;
        o[4] = p->chr[addr & 0x1FFF];
        run(p, 2);
        p->v = addr + 8;
        o[5] = p->chr[(addr + 8) & 0x1FFF];
        run(p, 2);
        if (!(o[2] & 0x40)) {
            o[4] = reverse8(o[4]);
            o[5] = reverse8(o[5]);
        }
    }
}

void ppun_run_frame(Ppu *p)
{
    int i, sl;

    if (p->dead_frames) {
        p->sl = 241;
        run(p, 20 * LINE_DOTS);
        p->sl = 0;
        run(p, 242 * LINE_DOTS);
        p->dead_frames--;
        return;
    }

    /* vblank: 20 lines */
    p->status |= 0x80;
    p->oam_addr = p->oam_low = 0;
    p->sl = 241;
    for (i = 0; i < 20; i++)
        run(p, 1);
    if (p->ctrl & 0x80)
        p->cpu->irq |= CPU_INT_NMI;
    for (sl = 0; sl < 20; sl++) {
        for (i = (sl == 0 ? 20 : 0); i < LINE_DOTS; i++)
            run(p, 1);
        p->sl++;
    }
    p->status = 0;

    /* pre-render line (sl 0) and 240 visible lines */
    for (sl = 0; sl < 241; sl++) {
        int yp = sl - 1, xt;
        int scanslot = p->oam_slot ^ 1, renderslot = p->oam_slot;

        p->sl = sl;
        p->scanline = yp; /* for debugging output only */
        p->oam_slot ^= 1;
        for (xt = 0; xt < 32; xt++) {
            fetch_tile(p, &p->bg[xt + 2]);
            if (sl != 0)
                draw_tile(p, yp, xt, renderslot);
        }
        if (sl != 0 && p->line_done) /* M2 hook: display only */
            p->line_done(p, yp, p->line_done_user);
        evaluate_sprites(p, yp, scanslot);
        fetch_sprites(p, sl, yp, scanslot);
        fetch_tile(p, &p->bg[0]);
        fetch_tile(p, &p->bg[1]);
        run(p, 2);
        if (sl == 0) {
            p->end_dot = (p->idle_synch && PPU_ON(p)) ? 340 : 341;
            p->idle_synch ^= 1;
        } else {
            p->end_dot = 341;
        }
        run(p, 2);
        if (p->end_dot == 341)
            run(p, 1);
    }

    /* idle line */
    run(p, LINE_DOTS);
}

void ppun_power(Ppu *p)
{
    p->sl = 241;
    p->dot = 0;
    p->end_dot = 341;
    p->idle_synch = 1;
}
