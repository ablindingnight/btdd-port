/* Play helpers (see play.h). */
#include "play.h"
#include <stdlib.h>
#include <string.h>

static Play *active; /* the watch hook has no user pointer of its own; one Play per process */

void play_init(Play *p)
{
    memset(p, 0, sizeof *p);
    p->warp_level = -1;
    p->warp_section = -1;
}

int play_parse_pokes(Play *p, const char *s)
{
    while (*s) {
        char *e;
        long a = strtol(s, &e, 16), v, fr = 0;
        int freeze = 0;
        if (*e != '=')
            return -1;
        v = strtol(e + 1, &e, 16);
        if (*e == '@') {
            fr = strtol(e + 1, &e, 10);
            if (*e == '+') {
                freeze = 1;
                e++;
            }
        }
        if (a < 0 || a >= 0x800 || v < 0 || v > 255 || p->npokes >= PLAY_MAX_POKES)
            return -1;
        p->pokes[p->npokes].addr = (uint16_t)a;
        p->pokes[p->npokes].value = (uint8_t)v;
        p->pokes[p->npokes].frame = fr;
        p->pokes[p->npokes].every_frame = (uint8_t)freeze;
        p->npokes++;
        if (*e == ',')
            e++;
        else if (*e)
            return -1;
        s = e;
    }
    return 0;
}

static void play_watch(Nes *n, uint16_t addr, uint8_t v)
{
    Play *p = active;
    if (p && addr == 0x0D && p->warp_level >= 0 && (p->warp_every || !p->warps_done) && n->prg_bank == 0 &&
        n->cpu.pc >= 0x8312 && n->cpu.pc <= 0x8319) {
        n->ram[0x10] = n->ram[0x0D] = (uint8_t)p->warp_level;
        if (p->warp_section >= 0)
            n->ram[0x15] = (uint8_t)p->warp_section;
        p->warps_done++;
    }
    if (p && p->next_watch && p->user_watch[addr & 0x7FF])
        p->next_watch(n, addr, v);
}

void play_install(Play *p, Nes *n)
{
    active = p;
    p->next_watch = n->watch_hook;
    memcpy(p->user_watch, n->watch, sizeof p->user_watch);
    n->watch[0x0D] = 1;
    n->watch_hook = play_watch;
}

void play_after_frame(Play *p, Nes *n, long done)
{
    int i;
    for (i = 0; i < p->npokes; i++) {
        PlayPoke *k = &p->pokes[i];
        if (done == k->frame || (k->every_frame && done > k->frame))
            n->ram[k->addr] = k->value;
    }
    if (p->god) {
        for (i = 0; i < 2; i++) {
            uint8_t *hp = &n->ram[0x51B + i], *lives = &n->ram[0x11 + i];
            if (*hp > 0 && *hp < 47)
                *hp = 47;
            if (*lives < 3)
                *lives = 3;
        }
    }
}
